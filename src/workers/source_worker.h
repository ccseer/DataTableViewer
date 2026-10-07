#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "core/table_source.h"
#include "parsers/sqlite_common.h"

Q_DECLARE_METATYPE(std::shared_ptr<const dtv::core::PageResult>)
Q_DECLARE_METATYPE(std::vector<dtv::core::ColumnMeta>)
Q_DECLARE_METATYPE(dtv::core::PageToken)
Q_DECLARE_METATYPE(dtv::core::RefetchKey)
Q_DECLARE_METATYPE(dtv::core::RefetchResult)

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
    void open(uint64_t viewGen, uint64_t opGen, const QString &path, const QString &tableName);
    void first(uint64_t viewGen, uint64_t opGen, int pageSize);
    void next(uint64_t viewGen, uint64_t opGen, const dtv::core::PageToken &token, int pageSize);
    void prev(uint64_t viewGen, uint64_t opGen, const dtv::core::PageToken &token, int pageSize);
    void last(uint64_t viewGen, uint64_t opGen, int pageSize, qint64 knownTotal);
    void sort(uint64_t viewGen, uint64_t opGen, size_t column, bool ascending);
    void refetch(uint64_t viewGen, uint64_t copyRequestId, const dtv::core::RefetchKey &key);
    void shutdown();

signals:
    void openCompleted(uint64_t viewGen, uint64_t opGen, bool ok, const QString &error,
                       const std::vector<dtv::core::ColumnMeta> &columns, bool canSort);
    void pageReady(uint64_t viewGen, uint64_t opGen,
                   std::shared_ptr<const dtv::core::PageResult> result);
    void sortCompleted(uint64_t viewGen, uint64_t opGen, bool ok, const QString &error,
                       qint64 total);
    void refetchCompleted(uint64_t viewGen, uint64_t copyRequestId,
                          const dtv::core::RefetchResult &result);

private:
    bool isStale(uint64_t viewGen, uint64_t opGen) const;
    bool isViewStale(uint64_t viewGen) const;
    void applyCancelCheck(uint64_t viewGen, std::optional<uint64_t> opGen = std::nullopt, uint64_t startSeq = 0);

    template <typename Func, typename IsCancelledFunc, typename IsStaleFunc>
    auto executeWithInterruptRetry(Func &&fn, IsCancelledFunc &&isCancelled, IsStaleFunc &&isStale);

    template <typename F>
    void executePageQuery(uint64_t viewGen, uint64_t opGen, F &&queryFunc);

    std::shared_ptr<std::atomic<uint64_t>> m_viewGen;
    std::shared_ptr<std::atomic<uint64_t>> m_opGen;
    std::unique_ptr<core::ITableSource> m_source;
    std::shared_ptr<parsers::InterruptHandle> m_interruptHandle;
    Hook m_progressHook;
    Hook m_interruptHook;
    Hook m_finishHook;
};

} // namespace dtv::workers
