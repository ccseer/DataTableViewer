#include <QtTest>
#include <sqlite3.h>
#include "parsers/sqlite_parser.h"

class TestSqliteParser : public QObject {
    Q_OBJECT
private slots:
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

        // 2. Get table data
        input.table_name = "items";
        result = parser.parse(input);
        if(!result.ok)
            qDebug() << "Phase 2 error:" << QString::fromStdString(result.error);
        QVERIFY(result.ok);
        QCOMPARE(result.data->columns.size(), 2ull);
        QCOMPARE(result.data->rows.size(), 2ull);
        QCOMPARE(result.data->rows[0][1], std::string("A"));

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
