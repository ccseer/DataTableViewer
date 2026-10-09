#include "source_worker.h"

#include "parsers/sqlite_common.h"
#include "parsers/sqlite_table_source.h"
#include "parsers/csv_file_source.h"

namespace dtv::workers {

void registerWorkerMetatypes() {
    static const bool registered = [] {
        qRegisterMetaType<std::shared_ptr<const dtv::core::PageResult>>(
            "std::shared_ptr<const dtv::core::PageResult>");
        qRegisterMetaType<std::vector<dtv::core::ColumnMeta>>(
            "std::vector<dtv::core::ColumnMeta>");
        qRegisterMetaType<dtv::core::PageToken>("dtv::core::PageToken");
        qRegisterMetaType<std::vector<std::pair<int, dtv::core::RefetchKey>>>(
            "std::vector<std::pair<int, dtv::core::RefetchKey>>");
        qRegisterMetaType<std::vector<std::pair<int, dtv::core::RefetchResult>>>(
            "std::vector<std::pair<int, dtv::core::RefetchResult>>");
        qRegisterMetaType<dtv::workers::SourceOpenDescriptor>(
            "dtv::workers::SourceOpenDescriptor");
        return true;
    }();
    Q_UNUSED(registered);
}

SourceWorker::SourceWorker(
    std::shared_ptr<std::atomic<uint64_t>> viewGen,
    std::shared_ptr<std::atomic<uint64_t>> opGen,
    std::unique_ptr<core::ITableSource> source,
    QObject *parent)
    : QObject(parent),
      m_viewGen(viewGen ? viewGen : std::make_shared<std::atomic<uint64_t>>(1)),
      m_opGen(opGen ? opGen : std::make_shared<std::atomic<uint64_t>>(1)),
      m_source(std::move(source))
{
    registerWorkerMetatypes();
    if (auto sqliteSource = dynamic_cast<parsers::SqliteTableSource*>(m_source.get())) {
        m_interruptHandle = sqliteSource->interruptHandle();
    }
    if (!m_interruptHandle) {
        m_interruptHandle = std::make_shared<parsers::InterruptHandle>();
    }
}

SourceWorker::~SourceWorker() {
    if (m_interruptHandle) {
        m_interruptHandle->clear();
    }
    m_source.reset();
}

std::shared_ptr<std::atomic<uint64_t>> SourceWorker::viewGen() const {
    return m_viewGen;
}

std::shared_ptr<std::atomic<uint64_t>> SourceWorker::opGen() const {
    return m_opGen;
}

std::shared_ptr<parsers::InterruptHandle> SourceWorker::interruptHandle() const {
    return m_interruptHandle;
}

void SourceWorker::interrupt() {
    if (m_interruptHandle) {
        m_interruptHandle->interrupt();
    }
}

void SourceWorker::setProgressHook(Hook hook) {
    m_progressHook = std::move(hook);
}

void SourceWorker::setInterruptHook(Hook hook) {
    m_interruptHook = std::move(hook);
}

void SourceWorker::setFinishHook(Hook hook) {
    m_finishHook = std::move(hook);
}

bool SourceWorker::isStale(uint64_t viewGen, uint64_t opGen) const {
    return (m_viewGen && m_viewGen->load() != viewGen) ||
           (m_opGen && m_opGen->load() != opGen);
}

bool SourceWorker::isViewStale(uint64_t viewGen) const {
    return m_viewGen && m_viewGen->load() != viewGen;
}

void SourceWorker::applyCancelCheck(uint64_t viewGen, std::optional<uint64_t> opGen, uint64_t startSeq) {
    if (!m_source) {
        return;
    }

    auto vGen = m_viewGen;
    auto oGen = m_opGen;
    auto handle = m_interruptHandle;
    auto hook = m_progressHook;
    m_source->setCancelCheck([vGen, oGen, handle, viewGen, opGen, startSeq, hook]() -> bool {
        if (hook) {
            try {
                hook();
            } catch(...) {
                return true;
            }
        }
        bool genStale = (vGen && vGen->load() != viewGen) ||
                        (opGen.has_value() && oGen && oGen->load() != *opGen);
        bool interrupted = (handle && handle->sequence() > startSeq);
        return genStale || interrupted;
    });
}

template <typename Func, typename IsCancelledFunc, typename IsStaleFunc>
auto SourceWorker::executeWithInterruptRetry(Func &&fn, IsCancelledFunc &&isCancelled, IsStaleFunc &&isStale) {
    uint64_t startSeq = m_interruptHandle ? m_interruptHandle->sequence() : 0;
    auto res = fn();
    if (isCancelled(res)) {
        uint64_t endSeq = m_interruptHandle ? m_interruptHandle->sequence() : 0;
        if (!isStale() && endSeq == startSeq) {
            res = fn();
        }
    }
    return res;
}

void SourceWorker::open(uint64_t viewGen, uint64_t opGen, const QString &path, const QString &tableName) {
    openDescriptor(viewGen, opGen, SourceOpenDescriptor{SourceKind::Sqlite, path, tableName, '\0'});
}

void SourceWorker::openDescriptor(uint64_t viewGen, uint64_t opGen, const dtv::workers::SourceOpenDescriptor &desc) {
    if (isStale(viewGen, opGen)) {
        return;
    }

    m_pendingPageRequest = std::nullopt;

    bool ok = false;
    std::string err;

    if (desc.kind == SourceKind::Sqlite) {
        if (!m_source || !dynamic_cast<parsers::SqliteTableSource*>(m_source.get())) {
            m_source = std::make_unique<parsers::SqliteTableSource>(m_interruptHandle);
        }

        uint64_t startSeq = m_interruptHandle ? m_interruptHandle->sequence() : 0;
        applyCancelCheck(viewGen, opGen, startSeq);

        auto sqliteSource = dynamic_cast<parsers::SqliteTableSource*>(m_source.get());
        if (sqliteSource) {
            ok = sqliteSource->open(desc.path.toStdString(), desc.tableName.toStdString());
            if (!ok) {
                err = sqliteSource->error();
            }
        } else {
            ok = false;
            err = "Source type mismatch";
        }
    } else { // SourceKind::Csv
        if (!m_source || !dynamic_cast<parsers::CsvFileSource*>(m_source.get())) {
            m_source = std::make_unique<parsers::CsvFileSource>(desc.delimiter);
        }

        uint64_t startSeq = m_interruptHandle ? m_interruptHandle->sequence() : 0;
        applyCancelCheck(viewGen, std::nullopt, startSeq);

        auto csvSource = dynamic_cast<parsers::CsvFileSource*>(m_source.get());
        if (csvSource) {
            ok = csvSource->open(desc.path.toStdString(), desc.delimiter);
            if (!ok) {
                err = csvSource->error();
            }
        } else {
            ok = false;
            err = "Source type mismatch";
        }
    }

    if (m_finishHook) {
        m_finishHook();
    }

    if (isStale(viewGen, opGen)) {
        return;
    }

    emit openCompleted(viewGen, opGen, ok, QString::fromStdString(err),
                       m_source ? m_source->columns() : std::vector<core::ColumnMeta>{},
                       m_source ? m_source->canSort() : false);

    if (ok && desc.kind == SourceKind::Csv && m_source) {
        if (m_source->rowCount().has_value()) {
            emit indexProgress(viewGen, *m_source->rowCount(), true, "");
        } else {
            m_progressTimer.start();
            QMetaObject::invokeMethod(this, "indexSlice", Qt::QueuedConnection, Q_ARG(uint64_t, viewGen));
        }
    }
}

void SourceWorker::indexSlice(uint64_t viewGen) {
    if (isViewStale(viewGen) || !m_source || !m_source->isIndexable() ||
        QThread::currentThread()->isInterruptionRequested()) {
        return;
    }

    uint64_t startSeq = m_interruptHandle ? m_interruptHandle->sequence() : 0;
    applyCancelCheck(viewGen, std::nullopt, startSeq);

    auto vGen = m_viewGen;
    auto handle = m_interruptHandle;
    auto cancelCheck = [vGen, viewGen, handle, startSeq]() -> bool {
        bool genStale = (vGen && vGen->load() != viewGen);
        bool interrupted = (handle && handle->sequence() > startSeq);
        return genStale || interrupted;
    };

    constexpr size_t kSliceBytes = 256 * 1024;
    auto progress = m_source->advanceIndex(kSliceBytes, cancelCheck);

    if (isViewStale(viewGen)) {
        return;
    }

    if (progress.isComplete || !progress.error.empty() || !m_progressTimer.isValid() || m_progressTimer.elapsed() >= 100) {
        m_progressTimer.restart();
        emit indexProgress(viewGen, progress.indexedRows, progress.isComplete, QString::fromStdString(progress.error));
    }

    // Check if pending page request can now be fulfilled or must fail
    if (m_pendingPageRequest.has_value()) {
        if (isStale(m_pendingPageRequest->viewGen, m_pendingPageRequest->opGen)) {
            m_pendingPageRequest.reset();
        } else if (!progress.error.empty()) {
            // Indexing encountered an error: reject pending page request immediately so UI exits busy state
            auto req = std::move(*m_pendingPageRequest);
            m_pendingPageRequest.reset();
            core::PageResult failureResult;
            failureResult.ok = false;
            failureResult.error = progress.error;
            emit pageReady(req.viewGen, req.opGen, std::make_shared<const core::PageResult>(std::move(failureResult)));
        } else {
            auto readyState = m_source->readiness(m_pendingPageRequest->firstOrdinal, m_pendingPageRequest->pageSize);
            if (readyState != core::IndexReadiness::Pending) {
                auto req = std::move(*m_pendingPageRequest);
                m_pendingPageRequest.reset();
                executePageQuery(req.viewGen, req.opGen, req.queryFunc);
            }
        }
    }

    if (!progress.isComplete && progress.error.empty() && !isViewStale(viewGen) &&
        !QThread::currentThread()->isInterruptionRequested()) {
        QMetaObject::invokeMethod(this, "indexSlice", Qt::QueuedConnection, Q_ARG(uint64_t, viewGen));
    }
}

template <typename F>
void SourceWorker::executePageQuery(uint64_t viewGen, uint64_t opGen, F &&queryFunc) {
    if (isStale(viewGen, opGen) || !m_source) {
        return;
    }
    uint64_t startSeq = m_interruptHandle ? m_interruptHandle->sequence() : 0;
    applyCancelCheck(viewGen, opGen, startSeq);

    auto res = executeWithInterruptRetry(
        queryFunc,
        [](const core::PageResult &r) { return !r.ok && core::isCancellationError(r.error); },
        [&]() { return isStale(viewGen, opGen); });

    if (m_finishHook) {
        m_finishHook();
    }

    if (isStale(viewGen, opGen)) {
        return;
    }
    emit pageReady(viewGen, opGen, std::make_shared<const core::PageResult>(std::move(res)));
}

void SourceWorker::first(uint64_t viewGen, uint64_t opGen, int pageSize) {
    if (isStale(viewGen, opGen) || !m_source) {
        return;
    }

    auto queryFunc = [this, pageSize]() { return m_source->first(pageSize); };

    if (m_source->isIndexable()) {
        auto readyState = m_source->readiness(0, pageSize);
        if (readyState == core::IndexReadiness::Pending) {
            m_pendingPageRequest = PendingPageRequest{viewGen, opGen, 0, pageSize, std::move(queryFunc)};
            return;
        }
    }

    m_pendingPageRequest.reset();
    executePageQuery(viewGen, opGen, queryFunc);
}

void SourceWorker::next(uint64_t viewGen, uint64_t opGen, const dtv::core::PageToken &token, int pageSize) {
    if (isStale(viewGen, opGen) || !m_source) {
        return;
    }

    auto queryFunc = [this, token, pageSize]() { return m_source->next(token, pageSize); };

    if (m_source->isIndexable()) {
        if (pageSize <= 0 || token.offset > (std::numeric_limits<int64_t>::max)() - pageSize) {
            m_pendingPageRequest.reset();
            executePageQuery(viewGen, opGen, queryFunc);
            return;
        }
        int64_t targetOrdinal = token.offset + pageSize;
        auto readyState = m_source->readiness(targetOrdinal, pageSize);
        if (readyState == core::IndexReadiness::Pending) {
            m_pendingPageRequest = PendingPageRequest{viewGen, opGen, targetOrdinal, pageSize, std::move(queryFunc)};
            return;
        }
    }

    m_pendingPageRequest.reset();
    executePageQuery(viewGen, opGen, queryFunc);
}

void SourceWorker::prev(uint64_t viewGen, uint64_t opGen, const dtv::core::PageToken &token, int pageSize) {
    if (isStale(viewGen, opGen) || !m_source) {
        return;
    }

    auto queryFunc = [this, token, pageSize]() { return m_source->prev(token, pageSize); };

    if (m_source->isIndexable()) {
        int64_t targetOrdinal = std::max<int64_t>(0, token.offset - pageSize);
        auto readyState = m_source->readiness(targetOrdinal, pageSize);
        if (readyState == core::IndexReadiness::Pending) {
            m_pendingPageRequest = PendingPageRequest{viewGen, opGen, targetOrdinal, pageSize, std::move(queryFunc)};
            return;
        }
    }

    m_pendingPageRequest.reset();
    executePageQuery(viewGen, opGen, queryFunc);
}

void SourceWorker::last(uint64_t viewGen, uint64_t opGen, int pageSize, qint64 knownTotal) {
    if (isStale(viewGen, opGen) || !m_source) {
        return;
    }

    auto queryFunc = [this, pageSize, knownTotal]() { return m_source->last(pageSize, knownTotal); };

    if (m_source->isIndexable()) {
        int64_t targetOrdinal = 0;
        if (pageSize > 0 && knownTotal > 0) {
            int64_t remainder = knownTotal % pageSize;
            targetOrdinal = remainder == 0 ? knownTotal - pageSize : knownTotal - remainder;
            targetOrdinal = std::max<int64_t>(0, targetOrdinal);
        }
        auto readyState = m_source->readiness(targetOrdinal, pageSize);
        if (readyState == core::IndexReadiness::Pending) {
            m_pendingPageRequest = PendingPageRequest{viewGen, opGen, targetOrdinal, pageSize, std::move(queryFunc)};
            return;
        }
    }

    m_pendingPageRequest.reset();
    executePageQuery(viewGen, opGen, queryFunc);
}

void SourceWorker::sort(uint64_t viewGen, uint64_t opGen, size_t column, bool ascending) {
    if (isStale(viewGen, opGen) || !m_source) {
        return;
    }
    uint64_t startSeq = m_interruptHandle ? m_interruptHandle->sequence() : 0;
    applyCancelCheck(viewGen, opGen, startSeq);

    if (m_interruptHook) {
        m_interruptHook();
    }

    auto sqliteSource = dynamic_cast<parsers::SqliteTableSource*>(m_source.get());
    std::string errStr;
    // errStr is captured by reference into the synchronous retry lambda,
    // reflecting the latest attempt's error string as required by the interrupt/retry contract.
    bool ok = executeWithInterruptRetry(
        [&]() {
            bool success = m_source->sort(column, ascending);
            if (sqliteSource) {
                errStr = sqliteSource->error();
            }
            if (!success && errStr.empty()) {
                // Only the SQLite source publishes a reason string. Any other
                // source reporting failure through this worker must still reach
                // the UI with something actionable, otherwise the status bar
                // renders a bare "Sort failed: ".
                errStr = m_source->canSort() ? "Sorting failed"
                                             : "Sorting is not supported by this source";
            }
            return success;
        },
        [&](bool success) {
            return !success && core::isCancellationError(errStr);
        },
        [&]() { return isStale(viewGen, opGen); });

    if (m_finishHook) {
        m_finishHook();
    }

    if (isStale(viewGen, opGen)) {
        return;
    }

    qint64 total = m_source->rowCount().value_or(0);
    QString err = ok ? QString() : QString::fromStdString(errStr);
    emit sortCompleted(viewGen, opGen, ok, err, total);
}

void SourceWorker::refetchRows(uint64_t viewGen, uint64_t copyRequestId,
                               const std::vector<std::pair<int, dtv::core::RefetchKey>> &rowKeys) {
    // A stale or interrupted view must still complete the renderer's pending
    // copy: it keeps its cells and would otherwise never receive a completion
    // or a failure, so the user sees a silent no-op.
    if (isViewStale(viewGen) || !m_source ||
        QThread::currentThread()->isInterruptionRequested()) {
        emit refetchRowsCompleted(viewGen, copyRequestId, {});
        return;
    }
    uint64_t startSeq = m_interruptHandle ? m_interruptHandle->sequence() : 0;
    applyCancelCheck(viewGen, std::nullopt, startSeq);

    size_t totalBatchBytes = 0;
    bool budgetExceeded = false;

    std::vector<std::pair<int, dtv::core::RefetchResult>> results;
    results.reserve(rowKeys.size());

    for (const auto &item : rowKeys) {
        if (isViewStale(viewGen)) {
            // The view moved on, so nothing may reach the clipboard any more.
            // Leaving without completing would strand the renderer's pending
            // copy: it keeps its cells and never gets a completion or a
            // failure, so the user sees a silent no-op.
            emit refetchRowsCompleted(viewGen, copyRequestId, {});
            return;
        }
        if (budgetExceeded) {
            core::RefetchResult failed;
            failed.ok = false;
            failed.error = core::kCopyBudgetExceededError;
            results.emplace_back(item.first, std::move(failed));
            continue;
        }

        auto res = executeWithInterruptRetry(
            [&]() { return m_source->refetch(item.second); },
            [](const core::RefetchResult &r) { return !r.ok && core::isCancellationError(r.error); },
            [&]() { return isViewStale(viewGen); });

        if (res.ok) {
            for (const auto &val : res.values) {
                totalBatchBytes += val.size();
            }
            if (totalBatchBytes > core::kMaxCopyBudgetBytes) {
                budgetExceeded = true;
            }
        }

        results.emplace_back(item.first, std::move(res));
    }

    if (budgetExceeded) {
        for (auto &entry : results) {
            entry.second.ok = false;
            entry.second.error = core::kCopyBudgetExceededError;
            entry.second.columns.clear();
            entry.second.values.clear();
        }
    }

    if (m_finishHook) {
        m_finishHook();
    }

    if (isViewStale(viewGen)) {
        return;
    }

    emit refetchRowsCompleted(viewGen, copyRequestId, results);
}

void SourceWorker::shutdown() {
    m_pendingPageRequest = std::nullopt;
    if (m_interruptHandle) {
        m_interruptHandle->clear();
    }
    m_source.reset();
    deleteLater();
}

} // namespace dtv::workers
