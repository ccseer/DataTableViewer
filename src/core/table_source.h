#pragma once
#include "table_data.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dtv::core {
using SqlValue =
    std::variant<std::monostate, int64_t, double, std::string, std::vector<unsigned char>>;
struct RefetchKey {
    std::optional<int64_t> rowid;
    std::vector<SqlValue> primaryKey;
    std::optional<int64_t> csvOrdinal;
    std::vector<int> selectedColumns;
};
struct PageToken {
    int64_t firstKey = 0;
    int64_t lastKey = 0;
    int64_t offset = 0;
    bool valid = false;
};
struct PageResult {
    bool ok = false;
    std::string error;
    std::shared_ptr<const TableData> data;
    std::vector<RefetchKey> keys;
    std::vector<std::vector<bool>> clamped;
    PageToken token;
    bool hasMore = false;
};
struct RefetchResult {
    bool ok = false;
    std::string error;
    std::vector<int> columns;
    std::vector<std::string> values;
};
// Aggregate budget for one copy batch: cell values plus the separators and
// markup the renderer adds. The source and the worker enforce the same limit,
// so both must read it from here rather than carry a private copy of the
// number. A batch that exceeds it fails as a whole: a partially serialized
// payload must never reach the clipboard.
inline constexpr std::size_t kMaxCopyBudgetBytes = 64 * 1024 * 1024;
inline constexpr char kCopyBudgetExceededError[] = "Copy budget exceeded (64 MiB)";
// Single spelling for cooperative cancellation. The worker recognizes a
// cancelled result by string, so a source reporting any other wording silently
// drops out of the interrupt/retry contract. "interrupted" is what SQLite puts
// in errmsg() after sqlite3_interrupt(), so both spellings are part of it.
inline constexpr char kCancelledError[] = "Cancelled";
inline constexpr char kInterruptedError[] = "interrupted";
inline bool isCancellationError(const std::string &error) {
    return error == kCancelledError || error == kInterruptedError;
}
enum class IndexReadiness {
    Ready,
    Pending,
    End,
    Failed
};
struct IndexProgress {
    int64_t indexedRows = 0;
    uint64_t scannedBytes = 0;
    bool isComplete = false;
    std::string error;
};
using CancelCheck = std::function<bool()>;
class ITableSource {
    public:
    virtual ~ITableSource() = default;
    virtual const std::vector<ColumnMeta> &columns() const = 0;
    virtual std::optional<int64_t> rowCount() const = 0;
    virtual void setKnownTotal(int64_t total) = 0;
    virtual PageResult first(int pageSize) = 0;
    virtual PageResult next(const PageToken &token, int pageSize) = 0;
    virtual PageResult prev(const PageToken &token, int pageSize) = 0;
    virtual PageResult last(int pageSize, std::optional<int64_t> knownTotal) = 0;
    virtual bool canSort() const = 0;
    virtual bool canSearch() const {
        return false;
    }
    virtual bool canRefetch() const = 0;
    virtual bool sort(size_t column, bool ascending, CancelCheck cancel = {}) = 0;
    virtual RefetchResult refetch(const RefetchKey &key) = 0;
    virtual bool isIndexable() const {
        return false;
    }
    virtual IndexReadiness readiness(int64_t /*firstOrdinal*/, int /*pageSize*/) const {
        return IndexReadiness::Ready;
    }
    virtual IndexProgress advanceIndex(size_t /*byteBudget*/, CancelCheck /*cancel*/ = {}) {
        return {};
    }
    virtual void setCancelCheck(CancelCheck /*cancel*/) {}
};
// One materialized page: navigation returns the same complete data, ignoring
// page size and anchors. hasMore is false; this adapter does not use a pager.
class MaterializedTableSource final : public ITableSource {
    public:
    explicit MaterializedTableSource(std::shared_ptr<const TableData> data);
    const std::vector<ColumnMeta> &columns() const override;
    std::optional<int64_t> rowCount() const override;
    void setKnownTotal(int64_t total) override;
    PageResult first(int) override;
    PageResult next(const PageToken &, int) override;
    PageResult prev(const PageToken &, int) override;
    PageResult last(int, std::optional<int64_t>) override;
    bool canSort() const override {
        return false;
    }
    bool canRefetch() const override {
        return false;
    }
    bool sort(size_t, bool, CancelCheck = {}) override {
        return false;
    }
    RefetchResult refetch(const RefetchKey &) override;

    private:
    std::shared_ptr<const TableData> m_data;
    std::optional<int64_t> m_total;
};
} // namespace dtv::core
