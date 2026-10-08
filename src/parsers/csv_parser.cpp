#include "csv_parser.h"
#include "csv_record_scanner.h"
#include "core/parser_registry.h"
#include <algorithm>

namespace dtv {
namespace parsers {

namespace {
constexpr size_t kMaxParserRows = 100000;
} // namespace

CsvParser::CsvParser(char delimiter) : m_delimiter(delimiter), m_autoDetect(delimiter == '\0')
{}

std::string CsvParser::format_name() const
{
    if(m_autoDetect)
        return "CSV/TSV";
    return m_delimiter == '\t' ? "TSV" : "CSV";
}

std::string CsvParser::library_credit() const
{
    return "(built-in RFC 4180 parser)";
}

char CsvParser::detectDelimiter(std::string_view bytes)
{
    return CsvRecordScanner::detectDelimiter(bytes);
}

core::TableParseResult CsvParser::parse(const core::ParseInput &in)
{
    core::TableParseResult result;
    if(in.bytes.empty()) {
        result.ok = false;
        result.error = "Empty file";
        return result;
    }

    char delim = m_autoDetect ? detectDelimiter(in.bytes) : m_delimiter;

    auto data = std::make_shared<core::TableData>();
    std::vector<std::string> currentRow;
    std::string currentCell;

    CsvRecordScanner::Callbacks cb;
    cb.onFieldFragment = [&](size_t colIndex, std::string_view fragment, bool isEnd) {
        if(currentRow.size() < CsvRecordScanner::kMaxColumns) {
            currentCell.append(fragment.data(), fragment.size());
            if(isEnd) {
                currentRow.push_back(std::move(currentCell));
                currentCell.clear();
            }
        }
    };

    cb.onRecord = [&](uint64_t /*recordOrdinal*/, uint64_t /*start*/, uint64_t /*end*/) {
        if(data->columns.empty()) {
            // First row is header
            for(size_t i = 0; i < currentRow.size(); ++i) {
                core::ColumnMeta meta;
                meta.name = currentRow[i];
                if(meta.name.empty())
                    meta.name = "Col" + std::to_string(i);
                data->columns.push_back(std::move(meta));
            }
        } else {
            if(data->rows.size() < kMaxParserRows) {
                data->rows.push_back(std::move(currentRow));
            }
            if(data->rows.size() >= kMaxParserRows) {
                data->truncated = true;
            }
        }
        currentRow.clear();
        currentCell.clear();
    };

    CsvRecordScanner scanner(delim, std::move(cb));
    bool feedOk = scanner.feed(in.bytes, 0, true, [&]() {
        return data->rows.size() >= kMaxParserRows;
    });

    if(!feedOk && !data->truncated) {
        result.ok = false;
        result.error = "Parse cancelled or failed";
        return result;
    }

    if(data->columns.empty() && data->rows.empty()) {
        result.ok = false;
        result.error = "No data found";
        return result;
    }

    // Ensure all rows have the same number of columns (fill with empty if needed)
    size_t colCount = data->columns.size();
    for(auto &row : data->rows) {
        if(row.size() < colCount)
            row.resize(colCount);
    }

    data->total_rows = data->rows.size();
    result.data = data;
    result.ok = true;

    return result;
}

} // namespace parsers
} // namespace dtv

REGISTER_TABLE_PARSER(csv, [] {
    return std::make_unique<dtv::parsers::CsvParser>();
})
REGISTER_TABLE_PARSER(tsv, [] {
    return std::make_unique<dtv::parsers::CsvParser>('\t');
})
