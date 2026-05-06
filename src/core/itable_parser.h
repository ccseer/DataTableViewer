#pragma once

#include <string>
#include <vector>
#include <string_view>
#include <memory>
#include <cstdint>
#include "table_data.h"

namespace dtv {
namespace core {

struct ParseInput {
    std::string_view bytes;      // CSV/TSV use this
    std::string_view file_path;  // Path-based parsers (SQLite) use this
    std::string_view table_name; // SQLite: empty -> return table list; non-empty -> fetch table
};

struct TableParseResult {
    std::shared_ptr<TableData> data;      // nullptr on error or table list
    std::vector<std::string> table_names; // SQLite table list
    bool ok = false;
    std::string error;
    std::string warning;
    int err_line = -1;

    // Filled by TableWorker:
    std::string format_name;
    std::string library_credit;
    std::int64_t file_bytes = 0;
    std::int64_t elapsed_ms = 0;
};

enum class ParseInputKind { Bytes, Path };

class ITableParser {
public:
    virtual ~ITableParser() = default;

    virtual TableParseResult parse(const ParseInput &in) = 0;
    virtual ParseInputKind input_kind() const = 0;
    virtual std::string format_name() const = 0;
    virtual std::string library_credit() const = 0;
};

} // namespace core
} // namespace dtv
