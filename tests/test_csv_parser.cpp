#include <QtTest>
#include "parsers/csv_parser.h"
#include "core/table_data.h"

class TestCsvParser : public QObject {
    Q_OBJECT
private slots:
    void testBasicCsv()
    {
        dtv::parsers::CsvParser parser;
        dtv::core::ParseInput input;
        input.bytes = "id,name,age\n1,Alice,30\n2,Bob,25";

        auto result = parser.parse(input);
        QVERIFY(result.ok);
        QCOMPARE(result.data->columns.size(), 3ull);
        QCOMPARE(result.data->rows.size(), 2ull);
        QCOMPARE(result.data->rows[0][1], std::string("Alice"));
    }

    void testQuotedCsv()
    {
        dtv::parsers::CsvParser parser;
        dtv::core::ParseInput input;
        input.bytes = "id,desc\n1,\"Hello, World\"\n2,\"Quoted \"\"internal\"\" quote\"";

        auto result = parser.parse(input);
        QVERIFY(result.ok);
        QCOMPARE(result.data->rows[0][1], std::string("Hello, World"));
        QCOMPARE(result.data->rows[1][1], std::string("Quoted \"internal\" quote"));
    }

    void testTsv()
    {
        dtv::parsers::CsvParser parser('\t');
        dtv::core::ParseInput input;
        input.bytes = "id\tname\n1\tAlice";

        auto result = parser.parse(input);
        QVERIFY(result.ok);
        QCOMPARE(result.data->columns.size(), 2ull);
        QCOMPARE(result.data->rows[0][1], std::string("Alice"));
    }

    void testFixtures_data()
    {
        QTest::addColumn<QString>("fileName");
        QTest::addColumn<bool>("shouldSucceed");

        QTest::newRow("basic") << "valid_basic.csv" << true;
        QTest::newRow("complex") << "valid_complex.csv" << true;
        QTest::newRow("diverse") << "valid_diverse.tsv" << true;
        QTest::newRow("broken_columns")
            << "broken_columns.csv" << true; // Parser should still succeed but maybe warn
        QTest::newRow("broken_quotes")
            << "broken_quotes.csv" << true; // Parser should handle gracefully
    }

    void testFixtures()
    {
        QFETCH(QString, fileName);
        QFETCH(bool, shouldSucceed);

        QString path = QString(FIXTURES_DIR) + "/" + fileName;
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable("Could not open " + path));

        QByteArray bytes = file.readAll();
        dtv::parsers::CsvParser parser(fileName.endsWith(".tsv") ? '\t' : ',');
        dtv::core::ParseInput input;
        input.bytes = std::string_view(bytes.constData(), bytes.size());

        auto result = parser.parse(input);
        QCOMPARE(result.ok, shouldSucceed);
    }
};

QTEST_GUILESS_MAIN(TestCsvParser)
#include "test_csv_parser.moc"
