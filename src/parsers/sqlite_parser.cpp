#include "sqlite_parser.h"
#include "core/parser_registry.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QSqlField>
#include <QSqlError>
#include <QUuid>
#include <QVariant>
#include <QDebug>
#include <QFile>
#include <limits>

namespace dtv {
namespace parsers {

namespace {
constexpr int kSoftRowLimit = 10000;
}

core::TableParseResult SqliteParser::parse(const core::ParseInput &in)
{
    core::TableParseResult result;
    const QString name =
        QStringLiteral("dtv_") + QUuid::createUuid().toString(QUuid::WithoutBraces);

    {
        QString path =
            QString::fromUtf8(in.file_path.data(), static_cast<int>(in.file_path.size()));
        if(!QFile::exists(path)) {
            result.ok = false;
            result.error = "File does not exist";
            return result;
        }

        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(path);

        if(!db.open()) {
            result.ok = false;
            result.error = db.lastError().text().toStdString();
            return result;
        }

        if(in.table_name.empty()) {
            // Phase 1: Enumerate tables
            QSqlQuery query(db);
            query.prepare("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE "
                          "'sqlite_%' ORDER BY name");
            if(query.exec()) {
                while(query.next()) {
                    result.table_names.push_back(query.value(0).toString().toStdString());
                }
                result.ok = true;
            } else {
                result.ok = false;
                result.error = query.lastError().text().toStdString();
            }
        } else {
            // Phase 2: Fetch table data
            QString tableName =
                QString::fromUtf8(in.table_name.data(), static_cast<int>(in.table_name.size()));
            // Escape double quotes in table name
            QString escaped = tableName.replace("\"", "\"\"");

            QSqlQuery query(db);
            // Limit kSoftRowLimit + 1 to detect truncation
            if(query.exec(
                   QString("SELECT * FROM \"%1\" LIMIT %2").arg(escaped).arg(kSoftRowLimit + 1))) {
                auto data = std::make_shared<core::TableData>();
                QSqlRecord rec = query.record();

                // Columns
                for(int i = 0; i < rec.count(); ++i) {
                    core::ColumnMeta meta;
                    meta.name = rec.fieldName(i).toStdString();

                    QMetaType type = rec.field(i).metaType();
                    switch(type.id()) {
                    case QMetaType::Int:
                    case QMetaType::LongLong:
                    case QMetaType::UInt:
                    case QMetaType::ULongLong:
                        meta.type = core::ColumnMeta::Type::Integer;
                        break;
                    case QMetaType::Double:
                    case QMetaType::Float:
                        meta.type = core::ColumnMeta::Type::Float;
                        break;
                    case QMetaType::Bool:
                        meta.type = core::ColumnMeta::Type::Boolean;
                        break;
                    default:
                        meta.type = core::ColumnMeta::Type::String;
                        break;
                    }
                    data->columns.push_back(std::move(meta));
                }

                // Rows
                int rowCount = 0;
                while(query.next()) {
                    if(rowCount >= kSoftRowLimit) {
                        data->truncated = true;
                        break;
                    }
                    std::vector<std::string> row;
                    for(int i = 0; i < rec.count(); ++i) {
                        QVariant val = query.value(i);
                        row.push_back(val.toString().toStdString());

                        // Fill numeric cache
                        if(data->columns[i].type == core::ColumnMeta::Type::Integer ||
                           data->columns[i].type == core::ColumnMeta::Type::Float) {
                            bool ok = false;
                            double d = val.toDouble(&ok);
                            data->numeric_cache.by_column[i].push_back(
                                ok ? d : std::numeric_limits<double>::quiet_NaN());
                        }
                    }
                    data->rows.push_back(std::move(row));
                    rowCount++;
                }

                data->total_rows = data->truncated ? 0 : data->rows.size();
                result.data = data;
                result.ok = true;
            } else {
                result.ok = false;
                result.error = query.lastError().text().toStdString();
            }
        }
    } // db goes out of scope here

    QSqlDatabase::removeDatabase(name);
    return result;
}

} // namespace parsers
} // namespace dtv

REGISTER_TABLE_PARSER(sqlite, [] {
    return std::make_unique<dtv::parsers::SqliteParser>();
})
REGISTER_TABLE_PARSER(sqlite3, [] {
    return std::make_unique<dtv::parsers::SqliteParser>();
})
REGISTER_TABLE_PARSER(db, [] {
    return std::make_unique<dtv::parsers::SqliteParser>();
})
REGISTER_TABLE_PARSER(db3, [] {
    return std::make_unique<dtv::parsers::SqliteParser>();
})
REGISTER_TABLE_PARSER(sl3, [] {
    return std::make_unique<dtv::parsers::SqliteParser>();
})
