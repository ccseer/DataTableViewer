#pragma once

#include "core/table_source.h"
#include "parsers/csv_record_index.h"
#include "parsers/csv_record_scanner.h"

#include <memory>
#include <string>
#include <vector>

namespace dtv::parsers {

class CsvFileSource final : public core::ITableSource {
public:
    explicit CsvFileSource(char delimiter = '\0');
    ~CsvFileSource() override;

    CsvFileSource(const CsvFileSource &) = delete;
    CsvFileSource &operator=(const CsvFileSource &) = delete;

    bool open(const std::string &path, char delimiter = '\0');
    const std::string &error() const
    {
        return m_error;
    }

    // Full path (UTF-8) of the backing temporary record index, for
    // diagnostics and tests. Empty before a successful open().
    std::string indexFilePath() const;

    const std::vector<core::ColumnMeta> &columns() const override;
    std::optional<int64_t> rowCount() const override;
    void setKnownTotal(int64_t total) override;

    core::PageResult first(int pageSize) override;
    core::PageResult next(const core::PageToken &token, int pageSize) override;
    core::PageResult prev(const core::PageToken &token, int pageSize) override;
    core::PageResult last(int pageSize, std::optional<int64_t> knownTotal) override;

    bool canSort() const override
    {
        return false;
    }
    bool canRefetch() const override
    {
        return true;
    }
    bool sort(size_t, bool, core::CancelCheck = {}) override
    {
        return false;
    }

    core::RefetchResult refetch(const core::RefetchKey &key) override;

    bool isIndexable() const override
    {
        return true;
    }
    core::IndexReadiness readiness(int64_t firstOrdinal, int pageSize) const override;
    core::IndexProgress advanceIndex(size_t byteBudget, core::CancelCheck cancel = {}) override;
    void setCancelCheck(core::CancelCheck cancel) override;

private:
    core::PageResult readPage(int64_t startOrdinal, int pageSize);
    size_t computePerCellCap(int pageSize, size_t pageColumnCount) const;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_error;
    char m_initialDelimiter{','};
};

} // namespace dtv::parsers
