#include "sqlite_parser.h"
#include "core/parser_registry.h"

#include <sqlite3.h>
#include <QFile>
#include <QDebug>
#include <limits>

namespace dtv {
namespace parsers {

namespace {
constexpr int kSoftRowLimit = 10000;

struct SqliteDeleter {
    void operator()(sqlite3 *db) const
    {
        sqlite3_close(db);
    }
    void operator()(sqlite3_stmt *stmt) const
    {
        sqlite3_finalize(stmt);
    }
};
using ScopedDb = std::unique_ptr<sqlite3, SqliteDeleter>;
using ScopedStmt = std::unique_ptr<sqlite3_stmt, SqliteDeleter>;

std::string escapeTableName(const std::string &name)
{
    std::string escaped = name;
    size_t pos = 0;
    while((pos = escaped.find('"', pos)) != std::string::npos) {
        escaped.replace(pos, 1, "\"\"");
        pos += 2;
    }
    return escaped;
}
} // namespace

core::TableParseResult SqliteParser::parse(const core::ParseInput &in)
{
    core::TableParseResult result;

    std::string path(in.file_path);
    sqlite3 *db_raw = nullptr;
    if(sqlite3_open_v2(path.c_str(), &db_raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        result.ok = false;
        result.error = db_raw ? sqlite3_errmsg(db_raw) : "Failed to open database";
        if(db_raw)
            sqlite3_close(db_raw);
        return result;
    }
    ScopedDb db(db_raw);

    if(in.table_name.empty()) {
        // Phase 1: Enumerate tables
        sqlite3_stmt *stmt_raw = nullptr;
        const char *sql = "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE "
                          "'sqlite_%' ORDER BY name";
        if(sqlite3_prepare_v2(db.get(), sql, -1, &stmt_raw, nullptr) == SQLITE_OK) {
            ScopedStmt stmt(stmt_raw);
            int rc;
            while((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
                const char *name =
                    reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 0));
                if(name) {
                    result.table_names.push_back(name);
                }
            }

            if(rc == SQLITE_DONE) {
                result.ok = true;
            } else {
                result.ok = false;
                result.error = sqlite3_errmsg(db.get());
            }
        } else {
            result.ok = false;
            result.error = sqlite3_errmsg(db.get());
        }
    } else {
        std::string table(in.table_name);
        std::string escaped = escapeTableName(table);
        std::string sql =
            "SELECT * FROM \"" + escaped + "\" LIMIT " + std::to_string(kSoftRowLimit + 1);

        sqlite3_stmt *stmt_raw = nullptr;
        if(sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &stmt_raw, nullptr) == SQLITE_OK) {
            ScopedStmt stmt(stmt_raw);
            auto data = std::make_shared<core::TableData>();
            int colCount = sqlite3_column_count(stmt.get());

            // Columns
            for(int i = 0; i < colCount; ++i) {
                core::ColumnMeta meta;
                meta.name = sqlite3_column_name(stmt.get(), i);

                // Note: column_decltype is better for empty tables,
                // but for SQLite, types are dynamic. We'll use decltype if available.
                const char *declType = sqlite3_column_decltype(stmt.get(), i);
                if(declType) {
                    std::string dt = declType;
                    for(auto &c : dt)
                        c = std::tolower(c);
                    if(dt.find("int") != std::string::npos) {
                        meta.type = core::ColumnMeta::Type::Integer;
                    } else if(dt.find("float") != std::string::npos ||
                              dt.find("double") != std::string::npos ||
                              dt.find("real") != std::string::npos) {
                        meta.type = core::ColumnMeta::Type::Float;
                    } else if(dt.find("bool") != std::string::npos) {
                        meta.type = core::ColumnMeta::Type::Boolean;
                    } else {
                        meta.type = core::ColumnMeta::Type::String;
                    }
                } else {
                    meta.type = core::ColumnMeta::Type::String; // Default
                }
                data->columns.push_back(std::move(meta));
            }

            // Rows
            int rowCount = 0;
            int rc;
            while((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
                if(rowCount >= kSoftRowLimit) {
                    data->truncated = true;
                    break;
                }
                std::vector<std::string> row;
                for(int i = 0; i < colCount; ++i) {
                    const char *text =
                        reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), i));
                    std::string cell = text ? text : "";
                    row.push_back(cell);

                    // Fill numeric cache
                    if(data->columns[i].type == core::ColumnMeta::Type::Integer ||
                       data->columns[i].type == core::ColumnMeta::Type::Float) {
                        int type = sqlite3_column_type(stmt.get(), i);
                        if(type == SQLITE_NULL) {
                            data->numeric_cache.by_column[i].push_back(
                                std::numeric_limits<double>::quiet_NaN());
                        } else {
                            data->numeric_cache.by_column[i].push_back(
                                sqlite3_column_double(stmt.get(), i));
                        }
                    }
                }
                data->rows.push_back(std::move(row));
                rowCount++;
            }

            if(rc == SQLITE_DONE || data->truncated) {
                data->total_rows = data->truncated ? 0 : data->rows.size();
                result.data = data;
                result.ok = true;
            } else {
                result.ok = false;
                result.error = sqlite3_errmsg(db.get());
            }
        } else {
            result.ok = false;
            result.error = sqlite3_errmsg(db.get());
        }
    }

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
