#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "parsers/sqlite_common.h"

namespace dtv::workers {

class CountWorker : public QObject {
    Q_OBJECT
public:
    explicit CountWorker(
        std::shared_ptr<std::atomic<uint64_t>> viewGen = nullptr,
        QObject *parent = nullptr);
    ~CountWorker() override;

    std::shared_ptr<std::atomic<uint64_t>> viewGen() const;
    std::shared_ptr<parsers::InterruptHandle> interruptHandle() const;
    void interrupt();

    using Hook = std::function<void()>;
    void setProgressHook(Hook hook);
    void setInterruptHook(Hook hook);
    void setFinishHook(Hook hook);

public slots:
    void count(uint64_t viewGen, const QString &path, const QString &tableName);
    void shutdown();

signals:
    void countCompleted(uint64_t viewGen, bool ok, const QString &error, qint64 total);

private:
    bool isViewStale(uint64_t viewGen) const;
    void closeDb();

    std::shared_ptr<std::atomic<uint64_t>> m_viewGen;
    std::shared_ptr<parsers::InterruptHandle> m_interruptHandle;
    sqlite3 *m_db = nullptr;
    std::string m_openedPath;
    Hook m_progressHook;
    Hook m_interruptHook;
    Hook m_finishHook;
};

} // namespace dtv::workers
