#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace dtv {
namespace core {

struct ColumnMeta {
    std::string name;
    enum class Type { String, Integer, Float, Boolean } type = Type::String;
};

// Sparse cache: only numeric columns get an entry; key = column index.
// NaN encodes "value not parseable as the column's numeric type".
struct NumericCache {
    std::unordered_map<int, std::vector<double>> by_column;
};

struct TableData {
    std::vector<ColumnMeta> columns;
    std::vector<std::vector<std::string>> rows; // already capped
    size_t total_rows = 0;                      // full count if known, else 0
    bool truncated = false;
    NumericCache numeric_cache; // populated by TypeInferrer for CSV/TSV
};

} // namespace core
} // namespace dtv
