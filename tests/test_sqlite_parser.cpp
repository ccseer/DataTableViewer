#include <QtTest>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include "parsers/sqlite_parser.h"

class TestSqliteParser : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        // Ensure Qt can find the SQL drivers
        QCoreApplication::addLibraryPath("C:/Users/corey/Dev/Qt/6.8.3/msvc2022_64/plugins");

        // Create a test database
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "test_db");
        db.setDatabaseName(":memory:");
        QVERIFY(db.open());

        QSqlQuery query(db);
        QVERIFY(query.exec("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)"));
        QVERIFY(query.exec("INSERT INTO users (name) VALUES ('Alice'), ('Bob')"));
        db.close();
    }

    void testBasicParse()
    {
        // We can't easily test the parser with :memory: because it re-opens the file
        // So we'll create a temporary file
        QString path = "test_parser.sqlite";
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "temp_db");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery query(db);
            query.exec("CREATE TABLE items (id INTEGER, val TEXT)");
            query.exec("INSERT INTO items VALUES (1, 'A'), (2, 'B')");
            db.close();
        }

        dtv::parsers::SqliteParser parser;
        dtv::core::ParseInput input;
        std::string pathStr = path.toStdString();
        input.file_path = pathStr;

        // 1. Get tables
        auto result = parser.parse(input);
        if(!result.ok)
            qDebug() << "Phase 1 error:" << QString::fromStdString(result.error);
        QVERIFY(result.ok);
        QVERIFY(result.data == nullptr);
        QCOMPARE(result.table_names.size(), 1ull); // One table: items
        QCOMPARE(result.table_names[0], std::string("items"));

        // 2. Get table data
        std::string tableName = "items";
        input.table_name = tableName;
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
        std::string invalidPath = "non_existent.sqlite";
        input.file_path = invalidPath;

        auto result = parser.parse(input);
        QVERIFY(!result.ok);
    }
};

QTEST_GUILESS_MAIN(TestSqliteParser)
#include "test_sqlite_parser.moc"
