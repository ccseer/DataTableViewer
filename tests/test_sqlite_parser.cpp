#include <QtTest>
#include <sqlite3.h>
#include "parsers/sqlite_parser.h"
#include "parsers/sqlite_common.h"

class TestSqliteParser : public QObject {
    Q_OBJECT
private slots:
    void testCorruptDatabase()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("corrupt.sqlite");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(4096, 'x')), qint64(4096));
        file.close();
        auto opened = dtv::parsers::openReadOnly(path.toUtf8().toStdString());
        const bool rejected = opened.db == nullptr;
        if(opened.db)
            sqlite3_close(opened.db);
        QVERIFY(rejected);
        QVERIFY(!opened.error.empty());
    }

    void testNativePaths_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("spaces") << QString("with spaces.sqlite");
        QTest::newRow("cjk") << QString::fromUtf8("\xE4\xB8\xAD\xE6\x96\x87.sqlite");
        QTest::newRow("hash") << QString("hash#.sqlite");
        QTest::newRow("percent") << QString("percent%.sqlite");
        QTest::newRow("mixed") << QString::fromUtf8("\xE4\xB8\xAD\xE6\x96\x87 #%.sqlite");
    }

    void testNativePaths()
    {
        QFETCH(QString, name);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::string path = dir.filePath(name).toUtf8().toStdString();
        sqlite3 *db = nullptr;
        QCOMPARE(sqlite3_open(path.c_str(), &db), SQLITE_OK);
        const int rc =
            sqlite3_exec(db, "CREATE TABLE items(value TEXT); INSERT INTO items VALUES('ok')",
                         nullptr, nullptr, nullptr);
        sqlite3_close(db);
        QCOMPARE(rc, SQLITE_OK);
        auto opened = dtv::parsers::openReadOnly(path);
        QVERIFY2(opened.db, opened.error.c_str());
        sqlite3_close(opened.db);
    }

    void testMissingFileNoFallback()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("missing.sqlite");
        auto opened = dtv::parsers::openReadOnly(path.toUtf8().toStdString());
        QVERIFY(!opened.db);
        QVERIFY(!opened.error.empty());
        QVERIFY(!QFile::exists(path));
    }

    void testBasicParse()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::string path = dir.filePath("test_parser.sqlite").toUtf8().toStdString();

        {
            sqlite3 *db = nullptr;
            QCOMPARE(sqlite3_open(path.c_str(), &db), SQLITE_OK);
            QCOMPARE(sqlite3_exec(db, "CREATE TABLE items (id INTEGER, val TEXT)", nullptr, nullptr,
                                  nullptr),
                     SQLITE_OK);
            QCOMPARE(sqlite3_exec(db, "INSERT INTO items VALUES (1, 'A'), (2, 'B')", nullptr,
                                  nullptr, nullptr),
                     SQLITE_OK);
            sqlite3_close(db);
        }

        dtv::parsers::SqliteParser parser;
        dtv::core::ParseInput input;
        input.file_path = path;

        // 1. Get tables
        auto result = parser.parse(input);
        if(!result.ok)
            qDebug() << "Phase 1 error:" << QString::fromStdString(result.error);
        QVERIFY(result.ok);
        QVERIFY(result.data == nullptr);
        QCOMPARE(result.table_names.size(), 1ull);
        QCOMPARE(result.table_names[0], std::string("items"));
    }

    void testTableNotesComeFromDdlComments()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::string path = dir.filePath("notes.sqlite").toUtf8().toStdString();
        {
            sqlite3 *db = nullptr;
            QCOMPARE(sqlite3_open(path.c_str(), &db), SQLITE_OK);
            QCOMPARE(sqlite3_exec(db,
                                  "CREATE TABLE /* stock ledger */ blocked(id INTEGER);"
                                  "CREATE TABLE lined -- monthly rollup\n(id INTEGER);"
                                  "CREATE TABLE quoted(id INTEGER, note TEXT DEFAULT '-- not a "
                                  "comment');",
                                  nullptr, nullptr, nullptr),
                     SQLITE_OK);
            sqlite3_close(db);
        }

        dtv::parsers::SqliteParser parser;
        dtv::core::ParseInput input;
        input.file_path = path;

        auto result = parser.parse(input);
        QVERIFY(result.ok);
        QCOMPARE(result.table_names.size(), 3ull);
        QCOMPARE(result.table_notes.size(), 3ull);

        // sqlite_master keeps the DDL as written, in name order. SQLite strips
        // block comments before storing it, so only a "--" comment survives.
        QCOMPARE(result.table_names[0], std::string("blocked"));
        QVERIFY(result.table_notes[0].empty());
        QCOMPARE(result.table_names[1], std::string("lined"));
        QCOMPARE(result.table_notes[1], std::string("monthly rollup"));
        // The only "--" in this table is inside a default value.
        QCOMPARE(result.table_names[2], std::string("quoted"));
        QVERIFY(result.table_notes[2].empty());
    }

    void testDatabaseWithoutUserTablesFails()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::string path = dir.filePath("no_tables.sqlite").toUtf8().toStdString();
        {
            sqlite3 *db = nullptr;
            QCOMPARE(sqlite3_open(path.c_str(), &db), SQLITE_OK);
            sqlite3_close(db);
        }

        dtv::parsers::SqliteParser parser;
        dtv::core::ParseInput input;
        input.file_path = path;

        auto result = parser.parse(input);
        // Reporting success here leaves the viewer with neither rows nor a
        // table picker, and therefore with no state to signal.
        QVERIFY(!result.ok);
        QVERIFY(!result.error.empty());
        QVERIFY(result.table_names.empty());
        QVERIFY(result.data == nullptr);
    }

    void testInvalidFile()
    {
        dtv::parsers::SqliteParser parser;
        dtv::core::ParseInput input;
        input.file_path = "non_existent_dir/non_existent.sqlite";

        auto result = parser.parse(input);
        QVERIFY(!result.ok);
    }
};

QTEST_GUILESS_MAIN(TestSqliteParser)
#include "test_sqlite_parser.moc"
