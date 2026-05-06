#include <QtTest>
#include "core/type_inferrer.h"
#include "core/table_data.h"
#include "parsers/csv_parser.h"

class TestTypeInferrer : public QObject {
    Q_OBJECT
private slots:
    void testInference()
    {
        dtv::core::TableData data;
        data.columns = {{"id", dtv::core::ColumnMeta::Type::String},
                        {"age", dtv::core::ColumnMeta::Type::String},
                        {"score", dtv::core::ColumnMeta::Type::String},
                        {"valid", dtv::core::ColumnMeta::Type::String}};

        data.rows = {{"1", "30", "95.5", "true"},
                     {"2", "25", "88.0", "false"},
                     {"3", "abc", "nan", "maybe"}};

        dtv::core::TypeInferrer::infer(data);

        QCOMPARE(data.columns[0].type, dtv::core::ColumnMeta::Type::Integer);
        QCOMPARE(data.columns[1].type, dtv::core::ColumnMeta::Type::String); // "abc" breaks integer
        QCOMPARE(data.columns[2].type,
                 dtv::core::ColumnMeta::Type::Float); // "nan" is handled in float
        QCOMPARE(data.columns[3].type,
                 dtv::core::ColumnMeta::Type::String); // "maybe" breaks boolean
    }

    void testInferenceFromFixture()
    {
        QString path = QString(FIXTURES_DIR) + "/valid_diverse.tsv";
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));

        QByteArray bytes = file.readAll();
        dtv::parsers::CsvParser parser('\t');
        dtv::core::ParseInput input;
        input.bytes = std::string_view(bytes.constData(), bytes.size());

        auto result = parser.parse(input);
        QVERIFY(result.ok);

        dtv::core::TypeInferrer::infer(*result.data);

        for(int i = 0; i < (int)result.data->columns.size(); ++i) {
            qDebug() << "Column" << i
                     << "name:" << QString::fromStdString(result.data->columns[i].name)
                     << "type:" << (int)result.data->columns[i].type;
        }

        // id: Integer, product: String, price: Float, is_available: Boolean, tags: String
        QCOMPARE(result.data->columns[0].type, dtv::core::ColumnMeta::Type::Integer);
        QCOMPARE(result.data->columns[1].type, dtv::core::ColumnMeta::Type::String);
        QCOMPARE(result.data->columns[2].type, dtv::core::ColumnMeta::Type::Float);
        QCOMPARE(result.data->columns[3].type, dtv::core::ColumnMeta::Type::Boolean);
        QCOMPARE(result.data->columns[4].type, dtv::core::ColumnMeta::Type::String);
    }
};

QTEST_GUILESS_MAIN(TestTypeInferrer)
#include "test_type_inferrer.moc"
