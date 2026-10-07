#include "count_worker.h"

#include <tuple>

namespace dtv::workers {

namespace {
struct StatementDeleter {
    void operator()(sqlite3_stmt *stmt) const {
        if(stmt) {
            sqlite3_finalize(stmt);
        }
    }
};
using Statement = std::unique_ptr<sqlite3_stmt, StatementDeleter>;
} // namespace

CountWorker::CountWorker(
    std::shared_ptr<std::atomic<uint64_t>> viewGen,
    QObject *parent)
    : QObject(parent),
      m_viewGen(viewGen ? viewGen : std::make_shared<std::atomic<uint64_t>>(1)),
      m_interruptHandle(std::make_shared<parsers::InterruptHandle>())
{
}

CountWorker::~CountWorker() {
    closeDb();
}

std::shared_ptr<std::atomic<uint64_t>> CountWorker::viewGen() const {
    return m_viewGen;
}

std::shared_ptr<parsers::InterruptHandle> CountWorker::interruptHandle() const {
    return m_interruptHandle;
}

void CountWorker::interrupt() {
    if (m_interruptHandle) {
        m_interruptHandle->interrupt();
    }
}

void CountWorker::setProgressHook(Hook hook) {
    m_progressHook = std::move(hook);
}

void CountWorker::setInterruptHook(Hook hook) {
    m_interruptHook = std::move(hook);
}

void CountWorker::setFinishHook(Hook hook) {
    m_finishHook = std::move(hook);
}

bool CountWorker::isViewStale(uint64_t viewGen) const {
    return m_viewGen && m_viewGen->load() != viewGen;
}

void CountWorker::closeDb() {
    if (m_interruptHandle) {
        m_interruptHandle->clear();
    }
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
    m_openedPath.clear();
}

void CountWorker::shutdown() {
    closeDb();
    deleteLater();
}

void CountWorker::count(uint64_t viewGen, const QString &path, const QString &tableName) {
    if (isViewStale(viewGen)) {
        return;
    }

    std::string pathStd = path.toStdString();
    if (!m_db || m_openedPath != pathStd) {
        closeDb();
        auto opened = parsers::openReadOnly(pathStd);
        if (!opened.db) {
            if (!isViewStale(viewGen)) {
                emit countCompleted(viewGen, false, QString::fromStdString(opened.error), 0);
            }
            return;
        }
        m_db = opened.db;
        m_openedPath = pathStd;
        m_interruptHandle->setDb(m_db);
    }

    uint64_t startSeq = m_interruptHandle ? m_interruptHandle->sequence() : 0;

    struct ProgressCtx {
        std::shared_ptr<std::atomic<uint64_t>> viewGen;
        uint64_t targetGen;
        std::shared_ptr<parsers::InterruptHandle> handle;
        uint64_t startSeq;
        CountWorker::Hook *hook;
    };
    ProgressCtx ctx{m_viewGen, viewGen, m_interruptHandle, startSeq, &m_progressHook};

    auto progressFn = [](void *arg) -> int {
        auto *c = static_cast<ProgressCtx*>(arg);
        try {
            if (c->hook && *(c->hook)) {
                (*(c->hook))();
            }
            bool genStale = (c->viewGen && c->viewGen->load() != c->targetGen);
            bool interrupted = (c->handle && c->handle->sequence() > c->startSeq);
            if (genStale || interrupted) {
                return 1;
            }
            return 0;
        } catch(...) {
            return 1;
        }
    };

    sqlite3_progress_handler(m_db, 1000, progressFn, &ctx);

    std::string sql = "SELECT COUNT(*) FROM main.\"" +
                      parsers::escapeSqlIdentifier(tableName.toStdString()) + "\"";

    auto runCount = [&]() -> std::tuple<bool, QString, qint64, bool> {
        sqlite3_stmt *rawStmt = nullptr;
        int rc = sqlite3_prepare_v2(m_db, sql.c_str(), -1, &rawStmt, nullptr);
        Statement stmt(rawStmt);
        if (rc != SQLITE_OK) {
            bool isInt = (rc == SQLITE_INTERRUPT);
            return {false, QString::fromUtf8(sqlite3_errmsg(m_db)), 0, isInt};
        }
        if (m_interruptHook) {
            m_interruptHook();
        }
        rc = sqlite3_step(stmt.get());
        bool ok = false;
        qint64 total = 0;
        QString err;
        bool isInt = (rc == SQLITE_INTERRUPT);
        if (rc == SQLITE_ROW) {
            total = sqlite3_column_int64(stmt.get(), 0);
            ok = true;
        } else {
            err = QString::fromUtf8(sqlite3_errmsg(m_db));
        }
        return {ok, err, total, isInt};
    };

    auto [ok, err, total, isInt] = runCount();
    if (!ok && isInt) {
        uint64_t endSeq = m_interruptHandle ? m_interruptHandle->sequence() : 0;
        if (!isViewStale(viewGen) && endSeq == startSeq) {
            auto retry = runCount();
            ok = std::get<0>(retry);
            err = std::get<1>(retry);
            total = std::get<2>(retry);
        }
    }

    sqlite3_progress_handler(m_db, 0, nullptr, nullptr);

    if (m_finishHook) {
        m_finishHook();
    }

    if (isViewStale(viewGen)) {
        return;
    }

    emit countCompleted(viewGen, ok, err, total);
}

} // namespace dtv::workers
