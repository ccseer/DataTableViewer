#include "sqlite_parser.h"
#include "sqlite_common.h"
#include "core/parser_registry.h"

#include <sqlite3.h>

#include <string>
#include <string_view>

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

std::string trimmed(std::string_view text)
{
    const char *space = " \t\r\n";
    const size_t begin = text.find_first_not_of(space);
    if(begin == std::string_view::npos)
        return {};
    const size_t end = text.find_last_not_of(space);
    return std::string(text.substr(begin, end - begin + 1));
}

std::string extractTableNote(const char *sql_ddl)
{
    if(!sql_ddl)
        return {};
    const std::string s(sql_ddl);

    // A comment is only a comment outside a string literal. Scanning for "--"
    // or "/*" anywhere in the DDL would turn a value such as
    // DEFAULT '-- none' into the table's note.
    for(size_t i = 0; i < s.size(); ++i) {
        if(s[i] == '\'') {
            ++i;
            while(i < s.size()) {
                if(s[i] == '\'') {
                    // A quote written twice inside a literal escapes itself.
                    if(i + 1 < s.size() && s[i + 1] == '\'') {
                        ++i;
                    } else {
                        break;
                    }
                }
                ++i;
            }
            continue;
        }
        if(s[i] == '-' && i + 1 < s.size() && s[i + 1] == '-') {
            const size_t start = i + 2;
            const size_t end = s.find_first_of("\r\n", start);
            return trimmed(std::string_view(s).substr(start, end == std::string::npos
                                                                 ? std::string::npos
                                                                 : end - start));
        }
        // SQLite strips block comments out of sqlite_master.sql, so a stored
        // DDL reaching this branch is unlikely; it stays as a cheap guard for
        // any source that does keep them.
        if(s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            const size_t start = i + 2;
            const size_t end = s.find("*/", start);
            return trimmed(std::string_view(s).substr(start, end == std::string::npos
                                                                 ? std::string::npos
                                                                 : end - start));
        }
    }

    return {};
}

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
        const char *sql = "SELECT name, sql FROM sqlite_master WHERE type='table' AND name NOT LIKE "
                          "'sqlite_%' ORDER BY name";
        if(sqlite3_prepare_v2(db.get(), sql, -1, &stmt_raw, nullptr) == SQLITE_OK) {
            ScopedStmt stmt(stmt_raw);
            int rc;
            while((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
                const char *name =
                    reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 0));
                const char *sql_ddl =
                    reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 1));
                if(name) {
                    result.table_names.push_back(name);

                    std::string note = extractTableNote(sql_ddl);

                    // Extract columns, types, pk from PRAGMA table_info
                    std::string infoSql =
                        "PRAGMA table_info(\"" + escapeSqlIdentifier(name) + "\")";
                    sqlite3_stmt *infoStmt = nullptr;
                    std::vector<std::string> colSummary;
                    std::vector<std::string> pkCols;
                    if(sqlite3_prepare_v2(db.get(), infoSql.c_str(), -1, &infoStmt, nullptr) ==
                       SQLITE_OK) {
                        while(sqlite3_step(infoStmt) == SQLITE_ROW) {
                            const char *cName = reinterpret_cast<const char *>(
                                sqlite3_column_text(infoStmt, 1));
                            const char *cType = reinterpret_cast<const char *>(
                                sqlite3_column_text(infoStmt, 2));
                            int pk = sqlite3_column_int(infoStmt, 5);
                            if(cName) {
                                std::string def = cName;
                                if(cType && *cType) {
                                    def += " (" + std::string(cType) + ")";
                                }
                                if(pk > 0) {
                                    pkCols.push_back(cName);
                                    def += " [PK]";
                                }
                                colSummary.push_back(def);
                            }
                        }
                        sqlite3_finalize(infoStmt);
                    }

                    // Extract user-defined indexes from PRAGMA index_list
                    std::string idxSql =
                        "PRAGMA index_list(\"" + escapeSqlIdentifier(name) + "\")";
                    sqlite3_stmt *idxStmt = nullptr;
                    std::vector<std::string> idxNames;
                    if(sqlite3_prepare_v2(db.get(), idxSql.c_str(), -1, &idxStmt, nullptr) ==
                       SQLITE_OK) {
                        while(sqlite3_step(idxStmt) == SQLITE_ROW) {
                            const char *iName = reinterpret_cast<const char *>(
                                sqlite3_column_text(idxStmt, 1));
                            if(iName && std::string(iName).rfind("sqlite_autoindex", 0) != 0) {
                                idxNames.push_back(iName);
                            }
                        }
                        sqlite3_finalize(idxStmt);
                    }

                    result.table_notes.push_back(note);

                    // Assemble structured metadata text
                    std::string schemaSummary;
                    if(!colSummary.empty()) {
                        schemaSummary += "Columns (" + std::to_string(colSummary.size()) + "): ";
                        size_t maxShow = std::min<size_t>(colSummary.size(), 6);
                        for(size_t i = 0; i < maxShow; ++i) {
                            if(i > 0)
                                schemaSummary += ", ";
                            schemaSummary += colSummary[i];
                        }
                        if(colSummary.size() > maxShow) {
                            schemaSummary +=
                                " ... (+" + std::to_string(colSummary.size() - maxShow) + ")";
                        }
                    }
                    if(!idxNames.empty()) {
                        if(!schemaSummary.empty())
                            schemaSummary += "\n";
                        schemaSummary += "Indexes: ";
                        for(size_t i = 0; i < idxNames.size(); ++i) {
                            if(i > 0)
                                schemaSummary += ", ";
                            schemaSummary += idxNames[i];
                        }
                    }

                    result.table_schemas.push_back(schemaSummary);
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
