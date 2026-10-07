#include "sqlite_table_source.h"
#include "sqlite_common.h"
#include <sqlite3.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <limits>
#include <utility>

namespace dtv::parsers {
namespace {
struct StatementDeleter {
    void operator()(sqlite3_stmt *stmt) const {
        sqlite3_finalize(stmt);
    }
};
using Statement = std::unique_ptr<sqlite3_stmt, StatementDeleter>;
std::string quoted(const std::string &name) {
    return "\"" + escapeSqlIdentifier(name) + "\"";
}
std::string asciiLower(std::string value) {
    for(char &c : value)
        if(c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return value;
}
core::ColumnMeta::Type columnType(const char *value) {
    const auto mapped = mapSqliteDeclType(value);
    if(mapped == "integer")
        return core::ColumnMeta::Type::Integer;
    if(mapped == "float")
        return core::ColumnMeta::Type::Float;
    if(mapped == "boolean")
        return core::ColumnMeta::Type::Boolean;
    return core::ColumnMeta::Type::String;
}
std::string cellText(sqlite3_stmt *stmt, int column, bool clamp, bool &clamped) {
    clamped = false;
    if(sqlite3_column_type(stmt, column) == SQLITE_BLOB)
        return "[BLOB " + std::to_string(sqlite3_column_bytes(stmt, column)) + " bytes]";
    const auto *text = sqlite3_column_text(stmt, column);
    const size_t bytes = static_cast<size_t>(sqlite3_column_bytes(stmt, column));
    if(!text)
        return {};
    size_t end = bytes;
    if(clamp && sqlite3_column_type(stmt, column) == SQLITE_TEXT && bytes > 4096) {
        end = 4096;
        while(end > 0 && (text[end] & 0xc0) == 0x80)
            --end;
        clamped = true;
    }
    std::string result(reinterpret_cast<const char *>(text), end);
    if(clamped)
        result += "\xE2\x80\xA6";
    return result;
}
core::SqlValue sqlValue(sqlite3_stmt *stmt, int col) {
    switch(sqlite3_column_type(stmt, col)) {
    case SQLITE_NULL:
        return std::monostate{};
    case SQLITE_INTEGER:
        return static_cast<int64_t>(sqlite3_column_int64(stmt, col));
    case SQLITE_FLOAT:
        return sqlite3_column_double(stmt, col);
    case SQLITE_BLOB: {
        const auto *p = static_cast<const unsigned char *>(sqlite3_column_blob(stmt, col));
        const int size = sqlite3_column_bytes(stmt, col);
        return p && size ? std::vector<unsigned char>(p, p + size) : std::vector<unsigned char>{};
    }
    default: {
        const auto *p = reinterpret_cast<const char *>(sqlite3_column_text(stmt, col));
        return p ? std::string(p, sqlite3_column_bytes(stmt, col)) : std::string{};
    }
    }
}
int bindValue(sqlite3_stmt *stmt, int parameter, const core::SqlValue &value) {
    if(std::holds_alternative<std::monostate>(value))
        return sqlite3_bind_null(stmt, parameter);
    if(auto v = std::get_if<int64_t>(&value))
        return sqlite3_bind_int64(stmt, parameter, *v);
    if(auto v = std::get_if<double>(&value))
        return sqlite3_bind_double(stmt, parameter, *v);
    if(auto v = std::get_if<std::string>(&value))
        return sqlite3_bind_text64(stmt, parameter, v->data(), v->size(), SQLITE_TRANSIENT,
                                   SQLITE_UTF8);
    const auto &v = std::get<std::vector<unsigned char>>(value);
    if(v.empty())
        return sqlite3_bind_zeroblob(stmt, parameter, 0);
    return sqlite3_bind_blob64(stmt, parameter, v.data(), v.size(), SQLITE_TRANSIENT);
}
core::PageResult failure(const std::string &error) {
    core::PageResult result;
    result.error = error;
    return result;
}
bool validSize(int size) {
    return size > 0 && size <= 3000;
}
} // namespace

struct SqliteTableSource::Impl {
    sqlite3 *db = nullptr;
    std::string table;
    std::string rowid;
    std::string orderTable;
    std::string error;
    std::vector<core::ColumnMeta> columns;
    std::vector<std::pair<int, std::string>> primaryKey;
    std::optional<int64_t> total;
    core::CancelCheck cancel;
    ~Impl() {
        if(db)
            sqlite3_close(db);
    }
    static int progress(void *p) {
        auto &self = *static_cast<Impl *>(p);
        try {
            return self.cancel && self.cancel() ? 1 : 0;
        } catch(...) {
            return 1;
        }
    }
    bool cancelled() const {
        return cancel && cancel();
    }
    void installProgress() {
        sqlite3_progress_handler(db, 1000, progress, this);
    }
    Statement prepare(const std::string &sql) {
        sqlite3_stmt *raw = nullptr;
        const int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr);
        Statement stmt(raw);
        if(rc != SQLITE_OK) {
            error = sqlite3_errmsg(db);
            return {};
        }
        return stmt;
    }
    core::PageResult fetch(int size, int direction, const core::PageToken &anchor,
                           int64_t lastSize = 0) {
        if(!db || !validSize(size))
            return failure("Invalid source or page size");
        if(cancelled())
            return failure("Cancelled");
        const bool reverse = direction == -1 || direction == 2;
        const bool sorted = !orderTable.empty();
        int64_t offset = 0;
        std::string sql;
        const int64_t limit = direction == 2 ? lastSize : (reverse ? size : size + 1);
        if(sorted) {
            int64_t first = 1;
            if(direction == 1) {
                if(anchor.lastKey == std::numeric_limits<int64_t>::max())
                    return failure("Page anchor overflow");
                first = anchor.lastKey + 1;
            }
            if(direction == -1)
                first = std::max<int64_t>(1, anchor.firstKey - size);
            if(direction == 2)
                first = *total - lastSize + 1;
            const int64_t width = direction == -1 ? anchor.firstKey - first : limit;
            if(width > 0 && first > std::numeric_limits<int64_t>::max() - (width - 1))
                return failure("Page range overflow");
            sql = "SELECT m.*, m." + rowid + ", o.ord FROM temp." + quoted(orderTable) +
                  " o CROSS JOIN main." + quoted(table) + " m ON m." + rowid +
                  " = o.rid WHERE o.ord >= ?1 AND o.ord <= ?2 ORDER BY o.ord";
            auto stmt = prepare(sql);
            if(!stmt)
                return failure(error);
            sqlite3_bind_int64(stmt.get(), 1, first);
            sqlite3_bind_int64(stmt.get(), 2, width ? first + width - 1 : first - 1);
            return read(stmt.get(), size, false, offset, sorted);
        }
        sql = "SELECT *" + (rowid.empty() ? std::string{} : ", " + rowid) + " FROM main." +
              quoted(table);
        if(!rowid.empty()) {
            if(direction == 1)
                sql += " WHERE " + rowid + " > ?1";
            if(direction == -1)
                sql += " WHERE " + rowid + " < ?1";
            sql += " ORDER BY " + rowid + (reverse ? " DESC" : " ASC");
        } else {
            if(direction == 1) {
                if(anchor.offset > std::numeric_limits<int64_t>::max() - size)
                    return failure("Page offset overflow");
                offset = anchor.offset + size;
            }
            if(direction == -1)
                offset = std::max<int64_t>(0, anchor.offset - size);
            if(direction == 2)
                offset = *total - lastSize;
        }
        sql += " LIMIT " + std::to_string(limit);
        if(rowid.empty())
            sql += " OFFSET " + std::to_string(offset);
        auto stmt = prepare(sql);
        if(!stmt)
            return failure(error);
        if(!rowid.empty() && (direction == 1 || direction == -1))
            sqlite3_bind_int64(stmt.get(), 1, direction == 1 ? anchor.lastKey : anchor.firstKey);
        return read(stmt.get(), size, reverse && !rowid.empty(), offset, false);
    }
    core::PageResult read(sqlite3_stmt *stmt, int size, bool reverse, int64_t offset, bool sorted) {
        core::PageResult result;
        auto data = std::make_shared<core::TableData>();
        data->columns = columns;
        std::vector<int64_t> anchors;
        int rc;
        while((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            if(cancelled())
                return failure("Cancelled");
            if(data->rows.size() == static_cast<size_t>(size)) {
                result.hasMore = true;
                break;
            }
            std::vector<std::string> row;
            std::vector<bool> flags;
            for(size_t col = 0; col < columns.size(); ++col) {
                const int storageType = sqlite3_column_type(stmt, static_cast<int>(col));
                double numeric = std::numeric_limits<double>::quiet_NaN();
                if(storageType == SQLITE_INTEGER || storageType == SQLITE_FLOAT)
                    numeric = sqlite3_column_double(stmt, static_cast<int>(col));
                bool clamped;
                row.push_back(cellText(stmt, static_cast<int>(col), true, clamped));
                flags.push_back(clamped);
                if(columns[col].type == core::ColumnMeta::Type::Integer ||
                   columns[col].type == core::ColumnMeta::Type::Float) {
                    if(storageType == SQLITE_TEXT) {
                        const auto &text = row.back();
                        double parsed;
                        const auto conversion =
                            std::from_chars(text.data(), text.data() + text.size(), parsed);
                        if(conversion.ec == std::errc{} &&
                           conversion.ptr == text.data() + text.size())
                            numeric = parsed;
                    }
                    data->numeric_cache.by_column[static_cast<int>(col)].push_back(numeric);
                }
            }
            core::RefetchKey key;
            if(!rowid.empty()) {
                key.rowid = sqlite3_column_int64(stmt, static_cast<int>(columns.size()));
                anchors.push_back(
                    sorted ? sqlite3_column_int64(stmt, static_cast<int>(columns.size() + 1))
                           : *key.rowid);
            } else {
                for(const auto &pk : primaryKey)
                    key.primaryKey.push_back(sqlValue(stmt, pk.first));
            }
            result.keys.push_back(std::move(key));
            result.clamped.push_back(std::move(flags));
            data->rows.push_back(std::move(row));
        }
        if(rc != SQLITE_DONE && rc != SQLITE_ROW)
            return failure(sqlite3_errmsg(db));
        if(reverse) {
            std::reverse(data->rows.begin(), data->rows.end());
            std::reverse(result.keys.begin(), result.keys.end());
            std::reverse(result.clamped.begin(), result.clamped.end());
            std::reverse(anchors.begin(), anchors.end());
            for(auto &entry : data->numeric_cache.by_column)
                std::reverse(entry.second.begin(), entry.second.end());
        }
        result.token.offset = offset;
        result.token.valid = !data->rows.empty();
        if(!anchors.empty()) {
            result.token.firstKey = anchors.front();
            result.token.lastKey = anchors.back();
        }
        if(reverse)
            result.hasMore = result.token.valid;
        if(total && *total >= 0)
            data->total_rows = static_cast<size_t>(*total);
        result.data = std::move(data);
        result.ok = true;
        return result;
    }
};

SqliteTableSource::SqliteTableSource() : m_impl(std::make_unique<Impl>()) {
}
SqliteTableSource::~SqliteTableSource() = default;
bool SqliteTableSource::open(const std::string &path, const std::string &table) {
    m_impl = std::make_unique<Impl>();
    auto &s = *m_impl;
    auto opened = openReadOnly(path);
    if(!opened.db) {
        s.error = opened.error;
        return false;
    }
    s.db = opened.db;
    s.table = table;
    auto metadata = s.prepare("SELECT * FROM main." + quoted(table) + " LIMIT 0");
    if(!metadata)
        return false;
    const int count = sqlite3_column_count(metadata.get());
    for(int col = 0; col < count; ++col)
        s.columns.push_back({sqlite3_column_name(metadata.get(), col),
                             columnType(sqlite3_column_decltype(metadata.get(), col))});
    metadata.reset();
    for(const char *alias : {"rowid", "_rowid_", "oid"}) {
        const bool shadowed = std::any_of(s.columns.begin(), s.columns.end(), [&](const auto &col) {
            return asciiLower(col.name) == alias;
        });
        if(shadowed)
            continue;
        auto probe =
            s.prepare("SELECT " + std::string(alias) + " FROM main." + quoted(table) + " LIMIT 1");
        if(!probe)
            continue;
        const int rc = sqlite3_step(probe.get());
        if(rc == SQLITE_ROW && sqlite3_column_type(probe.get(), 0) == SQLITE_INTEGER) {
            s.rowid = alias;
            break;
        }
        if(rc == SQLITE_DONE) {
            s.rowid = alias;
            break;
        }
    }
    if(s.rowid.empty()) {
        auto info = s.prepare("PRAGMA main.table_xinfo(" + quoted(table) + ")");
        if(!info)
            return false;
        std::vector<std::pair<int, std::string>> ordered;
        bool allPrimaryKeyColumnsNotNull = true;
        int rc;
        while((rc = sqlite3_step(info.get())) == SQLITE_ROW) {
            const int pk = sqlite3_column_int(info.get(), 5);
            if(pk) {
                allPrimaryKeyColumnsNotNull &= sqlite3_column_int(info.get(), 3) != 0;
                const auto *colName =
                    reinterpret_cast<const char *>(sqlite3_column_text(info.get(), 1));
                if(!colName) {
                    s.error = "Cannot read primary-key column name";
                    return false;
                }
                ordered.emplace_back(pk, colName);
            }
        }
        if(rc != SQLITE_DONE) {
            s.error = sqlite3_errmsg(s.db);
            return false;
        }
        // Ordinary SQLite primary keys can contain duplicate NULL tuples.
        if(!allPrimaryKeyColumnsNotNull)
            ordered.clear();
        std::sort(ordered.begin(), ordered.end());
        for(const auto &pk : ordered) {
            auto col = std::find_if(s.columns.begin(), s.columns.end(), [&](const auto &c) {
                return c.name == pk.second;
            });
            if(col != s.columns.end())
                s.primaryKey.emplace_back(static_cast<int>(col - s.columns.begin()), pk.second);
        }
    }
    s.error.clear();
    s.installProgress();
    return true;
}
const std::string &SqliteTableSource::error() const {
    return m_impl->error;
}
const std::vector<core::ColumnMeta> &SqliteTableSource::columns() const {
    return m_impl->columns;
}
std::optional<int64_t> SqliteTableSource::rowCount() const {
    return m_impl->total;
}
void SqliteTableSource::setKnownTotal(int64_t total) {
    if(total >= 0)
        m_impl->total = total;
}
core::PageResult SqliteTableSource::first(int size) {
    return m_impl->fetch(size, 0, {});
}
core::PageResult SqliteTableSource::next(const core::PageToken &token, int size) {
    return token.valid ? m_impl->fetch(size, 1, token) : failure("Invalid page token");
}
core::PageResult SqliteTableSource::prev(const core::PageToken &token, int size) {
    return token.valid ? m_impl->fetch(size, -1, token) : failure("Invalid page token");
}
core::PageResult SqliteTableSource::last(int size, std::optional<int64_t> total) {
    if(!total || *total < 0 || !validSize(size))
        return failure("Last page requires a known total and valid size");
    setKnownTotal(*total);
    if(*total == 0)
        return first(size);
    const int64_t remainder = *total % size;
    auto result = m_impl->fetch(size, 2, {}, remainder ? remainder : size);
    result.hasMore = false;
    return result;
}
bool SqliteTableSource::canSort() const {
    return !m_impl->rowid.empty();
}
bool SqliteTableSource::canRefetch() const {
    return !m_impl->rowid.empty() || !m_impl->primaryKey.empty();
}
void SqliteTableSource::setCancelCheck(core::CancelCheck cancel) {
    m_impl->cancel = std::move(cancel);
}
bool SqliteTableSource::sort(size_t column, bool ascending, core::CancelCheck cancel) {
    auto &s = *m_impl;
    if(!canSort() || column >= s.columns.size()) {
        s.error = "Sorting unavailable or invalid column";
        return false;
    }
    auto previousCancel = s.cancel;
    if(cancel)
        s.cancel = [previousCancel, cancel] {
            return (previousCancel && previousCancel()) || cancel();
        };
    auto restore = [&] {
        s.cancel = previousCancel;
        s.installProgress();
    };
    if(s.cancelled()) {
        s.error = "Cancelled";
        restore();
        return false;
    }
    static std::atomic<uint64_t> sequence{0};
    const auto staging = "dtv_ord_" + std::to_string(++sequence);
    const auto direction = ascending ? " ASC" : " DESC";
    const auto create =
        "CREATE TEMP TABLE " + quoted(staging) + "(ord INTEGER PRIMARY KEY, rid INTEGER)";
    int rc = sqlite3_exec(s.db, create.c_str(), nullptr, nullptr, nullptr);
    if(rc == SQLITE_OK) {
        const auto insert = "INSERT INTO temp." + quoted(staging) +
                            "(ord,rid) SELECT ROW_NUMBER() OVER (ORDER BY " +
                            quoted(s.columns[column].name) + direction + ", " + s.rowid +
                            direction + "), " + s.rowid + " FROM main." + quoted(s.table);
        rc = sqlite3_exec(s.db, insert.c_str(), nullptr, nullptr, nullptr);
    }
    const bool cancelled = s.cancelled();
    if(rc != SQLITE_OK || cancelled) {
        s.error = cancelled ? "Cancelled" : sqlite3_errmsg(s.db);
        sqlite3_progress_handler(s.db, 0, nullptr, nullptr);
        const auto drop = "DROP TABLE IF EXISTS temp." + quoted(staging);
        sqlite3_exec(s.db, drop.c_str(), nullptr, nullptr, nullptr);
        restore();
        return false;
    }
    const int64_t total = sqlite3_changes64(s.db);
    const auto old = std::exchange(s.orderTable, staging);
    setKnownTotal(total);
    sqlite3_progress_handler(s.db, 0, nullptr, nullptr);
    if(!old.empty()) {
        const auto drop = "DROP TABLE temp." + quoted(old);
        sqlite3_exec(s.db, drop.c_str(), nullptr, nullptr, nullptr);
    }
    s.error.clear();
    restore();
    return true;
}
core::RefetchResult SqliteTableSource::refetch(const core::RefetchKey &key) {
    auto &s = *m_impl;
    core::RefetchResult result;
    if(!s.db || s.cancelled()) {
        result.error = "Unavailable or cancelled";
        return result;
    }
    std::string sql = "SELECT * FROM main." + quoted(s.table) + " WHERE ";
    std::vector<core::SqlValue> values;
    if(!s.rowid.empty() && key.rowid) {
        sql += s.rowid + " = ?1";
        values.emplace_back(*key.rowid);
    } else if(!s.primaryKey.empty() && key.primaryKey.size() == s.primaryKey.size()) {
        values = key.primaryKey;
        for(size_t i = 0; i < s.primaryKey.size(); ++i) {
            if(i)
                sql += " AND ";
            sql += quoted(s.primaryKey[i].second) + " IS ?" + std::to_string(i + 1);
        }
    } else {
        result.error = "Invalid refetch key";
        return result;
    }
    auto stmt = s.prepare(sql);
    if(!stmt) {
        result.error = s.error;
        return result;
    }
    for(size_t i = 0; i < values.size(); ++i) {
        if(bindValue(stmt.get(), static_cast<int>(i + 1), values[i]) != SQLITE_OK) {
            result.error = sqlite3_errmsg(s.db);
            return result;
        }
    }
    const int rc = sqlite3_step(stmt.get());
    if(rc != SQLITE_ROW) {
        result.error = rc == SQLITE_DONE ? "Row no longer exists" : sqlite3_errmsg(s.db);
        return result;
    }
    for(size_t i = 0; i < s.columns.size(); ++i) {
        bool ignored;
        result.values.push_back(cellText(stmt.get(), static_cast<int>(i), false, ignored));
    }
    result.ok = true;
    return result;
}
} // namespace dtv::parsers
