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
        QVERIFY(!opened.immutable);
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
        const int rc = sqlite3_exec(db, "CREATE TABLE items(value TEXT); INSERT INTO items VALUES('ok')",
                                    nullptr, nullptr, nullptr);
        sqlite3_close(db);
        QCOMPARE(rc, SQLITE_OK);
        auto opened = dtv::parsers::openReadOnly(path);
        QVERIFY2(opened.db, opened.error.c_str());
        sqlite3_close(opened.db);
        QVERIFY(!opened.uri_mode);
        QVERIFY(!opened.immutable);
        QCOMPARE(opened.effective_name, path);
    }

    void testMissingFileNoFallback()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("missing.sqlite");
        auto opened = dtv::parsers::openReadOnly(path.toUtf8().toStdString());
        QVERIFY(!opened.db);
        QVERIFY(!opened.error.empty());
        QVERIFY(!opened.immutable);
        QVERIFY(!QFile::exists(path));
    }

    void testBasicParse()
    {
        const char *path = "test_parser.sqlite";
        QFile::remove(path); // Ensure a fresh start [P2]

        {
            sqlite3 *db = nullptr;
            QCOMPARE(sqlite3_open(path, &db), SQLITE_OK);
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

        QFile::remove(path);
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
