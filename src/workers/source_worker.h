#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "core/table_source.h"
#include "parsers/sqlite_common.h"

namespace dtv::workers {

enum class SourceKind {
    Sqlite,
    Csv
};

struct SourceOpenDescriptor {
    SourceKind kind = SourceKind::Sqlite;
    QString path;
    QString tableName;
    char delimiter = '\0';
};

} // namespace dtv::workers

// Q_ARG and Q_DECLARE_METATYPE are macros: types with top-level commas
// (vector<pair<...>>) must go through an alias or the macro would split the
// template arguments.
using RefetchRowKeysList = std::vector<std::pair<int, dtv::core::RefetchKey>>;
using RefetchRowResultsList = std::vector<std::pair<int, dtv::core::RefetchResult>>;

Q_DECLARE_METATYPE(std::shared_ptr<const dtv::core::PageResult>)
Q_DECLARE_METATYPE(std::vector<dtv::core::ColumnMeta>)
Q_DECLARE_METATYPE(dtv::core::PageToken)
Q_DECLARE_METATYPE(dtv::workers::SourceOpenDescriptor)

namespace dtv::workers {

void registerWorkerMetatypes();

class SourceWorker : public QObject {
    Q_OBJECT
public:
    explicit SourceWorker(
        std::shared_ptr<std::atomic<uint64_t>> viewGen = nullptr,
        std::shared_ptr<std::atomic<uint64_t>> opGen = nullptr,
        std::unique_ptr<core::ITableSource> source = nullptr,
        QObject *parent = nullptr);
    ~SourceWorker() override;

    std::shared_ptr<std::atomic<uint64_t>> viewGen() const;
    std::shared_ptr<std::atomic<uint64_t>> opGen() const;
    std::shared_ptr<parsers::InterruptHandle> interruptHandle() const;
    void interrupt();

    using Hook = std::function<void()>;
    void setProgressHook(Hook hook);
    void setInterruptHook(Hook hook);
    void setFinishHook(Hook hook);

public slots:
    void openDescriptor(uint64_t viewGen, uint64_t opGen, const dtv::workers::SourceOpenDescriptor &desc);
    void open(uint64_t viewGen, uint64_t opGen, const QString &path, const QString &tableName);
    void first(uint64_t viewGen, uint64_t opGen, int pageSize);
    void next(uint64_t viewGen, uint64_t opGen, const dtv::core::PageToken &token, int pageSize);
    void prev(uint64_t viewGen, uint64_t opGen, const dtv::core::PageToken &token, int pageSize);
    void last(uint64_t viewGen, uint64_t opGen, int pageSize, qint64 knownTotal);
    void sort(uint64_t viewGen, uint64_t opGen, size_t column, bool ascending);
    void refetchRows(uint64_t viewGen, uint64_t copyRequestId,
                     const std::vector<std::pair<int, dtv::core::RefetchKey>> &rowKeys);
    void shutdown();
    void indexSlice(uint64_t viewGen);

signals:
    void openCompleted(uint64_t viewGen, uint64_t opGen, bool ok, const QString &error,
                       const std::vector<dtv::core::ColumnMeta> &columns, bool canSort);
    void pageReady(uint64_t viewGen, uint64_t opGen,
                   std::shared_ptr<const dtv::core::PageResult> result);
    void sortCompleted(uint64_t viewGen, uint64_t opGen, bool ok, const QString &error,
                       qint64 total);
    void refetchRowsCompleted(uint64_t viewGen, uint64_t copyRequestId,
                              const std::vector<std::pair<int, dtv::core::RefetchResult>> &results);
    void indexProgress(uint64_t viewGen, qint64 totalRows, bool isComplete, const QString &error);

private:
    bool isStale(uint64_t viewGen, uint64_t opGen) const;
    bool isViewStale(uint64_t viewGen) const;
    void applyCancelCheck(uint64_t viewGen, std::optional<uint64_t> opGen = std::nullopt, uint64_t startSeq = 0);

    template <typename Func, typename IsCancelledFunc, typename IsStaleFunc>
    auto executeWithInterruptRetry(Func &&fn, IsCancelledFunc &&isCancelled, IsStaleFunc &&isStale);

    template <typename F>
    void executePageQuery(uint64_t viewGen, uint64_t opGen, F &&queryFunc);

    struct PendingPageRequest {
        uint64_t viewGen = 0;
        uint64_t opGen = 0;
        int64_t firstOrdinal = 0;
        int pageSize = 0;
        std::function<core::PageResult()> queryFunc;
    };

    std::shared_ptr<std::atomic<uint64_t>> m_viewGen;
    std::shared_ptr<std::atomic<uint64_t>> m_opGen;
    std::unique_ptr<core::ITableSource> m_source;
    std::shared_ptr<parsers::InterruptHandle> m_interruptHandle;
    Hook m_progressHook;
    Hook m_interruptHook;
    Hook m_finishHook;

    QElapsedTimer m_progressTimer;
    std::optional<PendingPageRequest> m_pendingPageRequest;
};

} // namespace dtv::workers
