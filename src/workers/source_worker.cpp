#include "source_worker.h"

#include "parsers/sqlite_common.h"
#include "parsers/sqlite_table_source.h"

namespace dtv::workers {

void registerWorkerMetatypes() {
    static const bool registered = [] {
        qRegisterMetaType<std::shared_ptr<const dtv::core::PageResult>>(
            "std::shared_ptr<const dtv::core::PageResult>");
        qRegisterMetaType<std::vector<dtv::core::ColumnMeta>>(
            "std::vector<dtv::core::ColumnMeta>");
        qRegisterMetaType<dtv::core::PageToken>("dtv::core::PageToken");
        qRegisterMetaType<dtv::core::RefetchKey>("dtv::core::RefetchKey");
        qRegisterMetaType<dtv::core::RefetchResult>("dtv::core::RefetchResult");
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
    auto sqliteSource = dynamic_cast<parsers::SqliteTableSource*>(m_source.get());
    if (!sqliteSource) {
        return;
    }

    auto vGen = m_viewGen;
    auto oGen = m_opGen;
    auto handle = m_interruptHandle;
    auto hook = m_progressHook;
    sqliteSource->setCancelCheck([vGen, oGen, handle, viewGen, opGen, startSeq, hook]() -> bool {
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
    if (isStale(viewGen, opGen)) {
        return;
    }

    if (!m_source) {
        m_source = std::make_unique<parsers::SqliteTableSource>(m_interruptHandle);
    }

    uint64_t startSeq = m_interruptHandle ? m_interruptHandle->sequence() : 0;
    applyCancelCheck(viewGen, opGen, startSeq);

    auto sqliteSource = dynamic_cast<parsers::SqliteTableSource*>(m_source.get());
    bool ok = false;
    std::string err;
    if (sqliteSource) {
        ok = sqliteSource->open(path.toStdString(), tableName.toStdString());
        if (!ok) {
            err = sqliteSource->error();
        }
    } else {
        ok = true;
    }

    if (m_finishHook) {
        m_finishHook();
    }

    if (isStale(viewGen, opGen)) {
        return;
    }

    emit openCompleted(viewGen, opGen, ok, QString::fromStdString(err),
                       m_source->columns(), m_source->canSort());
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
        [](const core::PageResult &r) { return !r.ok && (r.error == "interrupted" || r.error == "Cancelled"); },
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
    executePageQuery(viewGen, opGen, [&]() { return m_source->first(pageSize); });
}

void SourceWorker::next(uint64_t viewGen, uint64_t opGen, const dtv::core::PageToken &token, int pageSize) {
    executePageQuery(viewGen, opGen, [&]() { return m_source->next(token, pageSize); });
}

void SourceWorker::prev(uint64_t viewGen, uint64_t opGen, const dtv::core::PageToken &token, int pageSize) {
    executePageQuery(viewGen, opGen, [&]() { return m_source->prev(token, pageSize); });
}

void SourceWorker::last(uint64_t viewGen, uint64_t opGen, int pageSize, qint64 knownTotal) {
    executePageQuery(viewGen, opGen, [&]() { return m_source->last(pageSize, knownTotal); });
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
    bool ok = executeWithInterruptRetry(
        [&]() {
            bool success = m_source->sort(column, ascending);
            errStr = sqliteSource ? sqliteSource->error() : "";
            return success;
        },
        [&](bool success) {
            return !success && (errStr == "interrupted" || errStr == "Cancelled");
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

void SourceWorker::refetch(uint64_t viewGen, uint64_t copyRequestId, const dtv::core::RefetchKey &key) {
    if (isViewStale(viewGen) || !m_source) {
        return;
    }
    uint64_t startSeq = m_interruptHandle ? m_interruptHandle->sequence() : 0;
    applyCancelCheck(viewGen, std::nullopt, startSeq);

    auto res = executeWithInterruptRetry(
        [&]() { return m_source->refetch(key); },
        [](const core::RefetchResult &r) { return !r.ok && (r.error == "interrupted" || r.error == "Cancelled"); },
        [&]() { return isViewStale(viewGen); });

    if (m_finishHook) {
        m_finishHook();
    }

    if (isViewStale(viewGen)) {
        return;
    }

    emit refetchCompleted(viewGen, copyRequestId, res);
}

void SourceWorker::shutdown() {
    if (m_interruptHandle) {
        m_interruptHandle->clear();
    }
    m_source.reset();
    deleteLater();
}

} // namespace dtv::workers
