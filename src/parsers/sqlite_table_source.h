#pragma once
#include "core/table_source.h"
#include <memory>
struct sqlite3;
namespace dtv::parsers {
class InterruptHandle;
// Thread-confined: all methods, including callback replacement, run on the
// owning worker thread. Cancellation callbacks may read an atomic token.
class SqliteTableSource final : public core::ITableSource {
    public:
    explicit SqliteTableSource(std::shared_ptr<InterruptHandle> handle = nullptr);
    ~SqliteTableSource() override;
    SqliteTableSource(const SqliteTableSource &) = delete;
    SqliteTableSource &operator=(const SqliteTableSource &) = delete;
    bool open(const std::string &path, const std::string &table);
    const std::string &error() const;
    const std::vector<core::ColumnMeta> &columns() const override;
    std::optional<int64_t> rowCount() const override;
    void setKnownTotal(int64_t total) override;
    core::PageResult first(int pageSize) override;
    core::PageResult next(const core::PageToken &, int pageSize) override;
    core::PageResult prev(const core::PageToken &, int pageSize) override;
    core::PageResult last(int pageSize, std::optional<int64_t> knownTotal) override;
    bool canSort() const override;
    bool canRefetch() const override;
    bool sort(size_t column, bool ascending, core::CancelCheck cancel = {}) override;
    core::RefetchResult refetch(const core::RefetchKey &) override;
    void setCancelCheck(core::CancelCheck cancel);
    std::shared_ptr<InterruptHandle> interruptHandle() const;
    void interrupt();

    private:
    struct Impl;
    std::shared_ptr<InterruptHandle> m_interruptHandle;
    std::unique_ptr<Impl> m_impl;
};
} // namespace dtv::parsers
