#pragma once

#include <sqlite3.h>

#include <string>

namespace dtv::parsers {

struct SqliteOpenResult {
    sqlite3 *db = nullptr;
    bool uri_mode = false;
    bool immutable = false;
    std::string effective_name;
    std::string error;
};

std::string escapeSqlIdentifier(const std::string &name);
std::string mapSqliteDeclType(const char *decl_type);
SqliteOpenResult openReadOnly(const std::string &path);

} // namespace dtv::parsers
