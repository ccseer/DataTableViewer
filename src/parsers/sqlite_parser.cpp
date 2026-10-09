#include "sqlite_parser.h"
#include "sqlite_common.h"
#include "core/parser_registry.h"

#include <sqlite3.h>

namespace dtv {
namespace parsers {

namespace {
struct SqliteDeleter {
    void operator()(sqlite3 *db) const
    {
        sqlite3_close_v2(db);
    }
    void operator()(sqlite3_stmt *stmt) const
    {
        sqlite3_finalize(stmt);
    }
};
using ScopedDb = std::unique_ptr<sqlite3, SqliteDeleter>;
using ScopedStmt = std::unique_ptr<sqlite3_stmt, SqliteDeleter>;

} // namespace

core::TableParseResult SqliteParser::parse(const core::ParseInput &in)
{
    core::TableParseResult result;

    std::string path(in.file_path);
    auto opened = openReadOnly(path);
    sqlite3 *db_raw = opened.db;
    if(!db_raw) {
        result.ok = false;
        result.error = opened.error.empty() ? "Failed to open database" : opened.error;
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
                // A database with no user tables leaves the viewer with neither
                // data nor a picker, so it has to fail instead of reporting
                // success with an empty result set.
                result.ok = !result.table_names.empty();
                if(!result.ok)
                    result.error = "No user tables found in this SQLite database";
            } else {
                result.ok = false;
                result.error = sqlite3_errmsg(db.get());
            }
        } else {
            result.ok = false;
            result.error = sqlite3_errmsg(db.get());
        }
    } else {
        result.ok = false;
        result.error = "SQLite table data is loaded by the paged table source";
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
