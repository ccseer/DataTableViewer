#include "type_inferrer.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string_view>

namespace dtv {
namespace core {
namespace TypeInferrer {

namespace {

constexpr int kInferenceSampleRows = 200;

bool isBoolean(std::string_view s)
{
    if(s.empty())
        return true; // Skip empty
    std::string lower;
    lower.reserve(s.size());
    for(char c : s)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower == "true" || lower == "false" || lower == "1" || lower == "0" || lower == "yes" ||
           lower == "no";
}

bool isInteger(std::string_view s)
{
    if(s.empty())
        return true;
    size_t start = 0;
    if(s[0] == '-' || s[0] == '+') {
        if(s.size() == 1)
            return false;
        start = 1;
    }
    for(size_t i = start; i < s.size(); ++i) {
        if(!std::isdigit(static_cast<unsigned char>(s[i])))
            return false;
    }
    return true;
}

bool isFloat(std::string_view s, bool &outHasDecimalOrExponent)
{
    if(s.empty())
        return true;

    // Handle nan/inf
    if(s.size() >= 3) {
        std::string lower;
        lower.reserve(s.size());
        for(char c : s)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if(lower == "nan" || lower == "inf" || lower == "-inf" || lower == "+inf" ||
           lower == "infinity" || lower == "-infinity") {
            outHasDecimalOrExponent = true;
            return true;
        }
    }

    bool hasDecimal = false;
    bool hasExponent = false;
    bool hasDigits = false;

    size_t i = 0;
    if(s[i] == '-' || s[i] == '+') {
        i++;
    }

    for(; i < s.size(); ++i) {
        char c = s[i];
        if(std::isdigit(static_cast<unsigned char>(c))) {
            hasDigits = true;
        } else if(c == '.') {
            if(hasDecimal || hasExponent)
                return false;
            hasDecimal = true;
        } else if(c == 'e' || c == 'E') {
            if(hasExponent || !hasDigits)
                return false;
            hasExponent = true;
            if(i + 1 < s.size() && (s[i + 1] == '-' || s[i + 1] == '+')) {
                i++;
            }
        } else {
            return false;
        }
    }

    outHasDecimalOrExponent = hasDecimal || hasExponent;
    return hasDigits;
}

} // namespace

void infer(TableData &data)
{
    if(data.columns.empty())
        return;

    const int rowCount = static_cast<int>(data.rows.size());
    const int sampleCount = std::min(rowCount, kInferenceSampleRows);

    for(int colIdx = 0; colIdx < static_cast<int>(data.columns.size()); ++colIdx) {
        bool allBool = true;
        bool allInt = true;
        bool allFloat = true;
        bool hasAtLeastOneNonEmpty = false;
        bool floatHasDecimalOrExponent = false;

        for(int i = 0; i < sampleCount; ++i) {
            const std::string &cell = data.rows[i][colIdx];
            if(cell.empty())
                continue;

            hasAtLeastOneNonEmpty = true;

            if(allBool && !isBoolean(cell))
                allBool = false;
            if(allInt && !isInteger(cell))
                allInt = false;

            bool currentFloatHasMarker = false;
            if(allFloat && !isFloat(cell, currentFloatHasMarker)) {
                allFloat = false;
            } else if(currentFloatHasMarker) {
                floatHasDecimalOrExponent = true;
            }
        }

        if(!hasAtLeastOneNonEmpty) {
            data.columns[colIdx].type = ColumnMeta::Type::String;
        } else if(allBool) {
            data.columns[colIdx].type = ColumnMeta::Type::Boolean;
        } else if(allInt) {
            data.columns[colIdx].type = ColumnMeta::Type::Integer;
        } else if(allFloat && floatHasDecimalOrExponent) {
            data.columns[colIdx].type = ColumnMeta::Type::Float;
        } else {
            data.columns[colIdx].type = ColumnMeta::Type::String;
        }

        // Populate numeric cache if applicable
        if(data.columns[colIdx].type == ColumnMeta::Type::Integer ||
           data.columns[colIdx].type == ColumnMeta::Type::Float) {
            auto &cache = data.numeric_cache.by_column[colIdx];
            cache.reserve(rowCount);
            for(int i = 0; i < rowCount; ++i) {
                const std::string &cell = data.rows[i][colIdx];
                if(cell.empty()) {
                    cache.push_back(std::numeric_limits<double>::quiet_NaN());
                    continue;
                }

                try {
                    char *end = nullptr;
                    double val = std::strtod(cell.c_str(), &end);
                    if(end == cell.c_str() + cell.size()) {
                        cache.push_back(val);
                    } else {
                        cache.push_back(std::numeric_limits<double>::quiet_NaN());
                    }
                } catch(...) {
                    cache.push_back(std::numeric_limits<double>::quiet_NaN());
                }
            }
        }
    }
}

} // namespace TypeInferrer
} // namespace core
} // namespace dtv
