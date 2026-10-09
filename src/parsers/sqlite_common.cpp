#include "sqlite_common.h"

#include <algorithm>
#include <cctype>

namespace dtv::parsers {

std::string escapeSqlIdentifier(const std::string &name)
{
    std::string escaped = name;
    size_t pos = 0;
    while((pos = escaped.find('"', pos)) != std::string::npos) {
        escaped.replace(pos, 1, "\"\"");
        pos += 2;
    }
    return escaped;
}

std::string mapSqliteDeclType(const char *decl_type)
{
    if(!decl_type)
        return "string";
    std::string type(decl_type);
    std::transform(type.begin(), type.end(), type.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if(type.find("int") != std::string::npos)
        return "integer";
    if(type.find("float") != std::string::npos || type.find("double") != std::string::npos ||
       type.find("real") != std::string::npos)
        return "float";
    if(type.find("bool") != std::string::npos)
        return "boolean";
    return "string";
}

SqliteOpenResult openReadOnly(const std::string &path)
{
    SqliteOpenResult result;
    sqlite3 *db = nullptr;
    const int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX;
    int rc = sqlite3_open_v2(path.c_str(), &db, flags, nullptr);
    if(rc != SQLITE_OK) {
        result.error = db ? sqlite3_errmsg(db) : "Failed to open database";
        if(db)
            sqlite3_close_v2(db);
        return result;
    }

    // Execute the probe: prepare alone does not verify schema readability.
    sqlite3_stmt *stmt = nullptr;
    rc = sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master LIMIT 1", -1, &stmt, nullptr);
    if(rc == SQLITE_OK)
        rc = sqlite3_step(stmt);
    if(rc != SQLITE_ROW && rc != SQLITE_DONE) {
        result.error = sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        sqlite3_close_v2(db);
        return result;
    }
    sqlite3_finalize(stmt);
    // temp_store = FILE keeps the server-side sort's ordinal table on disk
    // instead of letting a large ORDER BY grow the process heap.
    rc = sqlite3_exec(db, "PRAGMA cache_size = -8000; PRAGMA mmap_size = 0; "
                          "PRAGMA temp_store = FILE",
                      nullptr, nullptr, nullptr);
    if(rc != SQLITE_OK) {
        result.error = sqlite3_errmsg(db);
        sqlite3_close_v2(db);
        return result;
    }
    result.db = db;
    return result;
}

} // namespace dtv::parsers
