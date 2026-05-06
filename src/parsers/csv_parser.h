#pragma once

#include "core/itable_parser.h"

namespace dtv {
namespace parsers {

class CsvParser : public core::ITableParser {
public:
    explicit CsvParser(char delimiter = '\0');

    core::TableParseResult parse(const core::ParseInput &in) override;
    core::ParseInputKind input_kind() const override
    {
        return core::ParseInputKind::Bytes;
    }
    std::string format_name() const override;
    std::string library_credit() const override;

private:
    char detectDelimiter(std::string_view bytes);

    char m_delimiter;
    bool m_autoDetect;
};

} // namespace parsers
} // namespace dtv
