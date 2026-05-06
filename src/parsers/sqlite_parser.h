#pragma once

#include "core/itable_parser.h"

namespace dtv {
namespace parsers {

class SqliteParser : public core::ITableParser {
public:
    SqliteParser() = default;

    core::TableParseResult parse(const core::ParseInput &in) override;
    core::ParseInputKind input_kind() const override
    {
        return core::ParseInputKind::Path;
    }
    std::string format_name() const override
    {
        return "SQLite";
    }
    std::string library_credit() const override
    {
        return "Qt QSQLITE driver";
    }
};

} // namespace parsers
} // namespace dtv
