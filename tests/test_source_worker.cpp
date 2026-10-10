#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <chrono>
#include <memory>
#include <sqlite3.h>

#include "workers/background_thread.h"
#include "workers/count_worker.h"
#include "workers/source_worker.h"
#include "parsers/sqlite_table_source.h"

using namespace dtv;
using namespace dtv::workers;

class TestSourceWorker : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testStartupAndOpen();
    void testFirstPage();
    void testNextPage();
    void testPrevPage();
    void testLastPage();
    void testStaleQueuedRequestDiscarded();
    void testViewGenCancellation();
    void testOpGenCancellation();
    void testCountProceedingWhileNavigationContinues();
    void testCountCancellationAndLatency();
    void testSortCancellation();
    void testInterruptRetryWhenCurrent();
    void testCancelInFlightWithoutGenBumpDoesNotRetry();
    void testOpenPreservesCancelCheckAndAborts();
    void testAbortedOpenClosesDbAndReleasesLock();
    void testShutdownAndConnectionCleanup();
    void testCsvOpenAndFirstPage();
    void testCsvPendingPageWaitAndFulfillment();
    void testCsvOpGenSupersedesPendingRequestLeavesIndexingAlive();
    void testCsvViewGenCancellationStopsIndexing();
    void testRefetchRowsBatchBudget();
    void testRefetchRowsCompletesAfterRetiredView();
    void testRefetchRowsCompletesAfterInterruption();
    void testSortFailureReportsReasonForNonSqliteSource();

signals:
    void reqOpenDescriptor(uint64_t viewGen, uint64_t opGen,
                           const dtv::workers::SourceOpenDescriptor &desc);
    void reqOpen(uint64_t viewGen, uint64_t opGen, const QString &path, const QString &tableName);
    void reqFirst(uint64_t viewGen, uint64_t opGen, int pageSize);
    void reqNext(uint64_t viewGen, uint64_t opGen, const dtv::core::PageToken &token, int pageSize);
    void reqPrev(uint64_t viewGen, uint64_t opGen, const dtv::core::PageToken &token, int pageSize);
    void reqLast(uint64_t viewGen, uint64_t opGen, int pageSize, qint64 knownTotal);
    void reqSort(uint64_t viewGen, uint64_t opGen, size_t column, bool ascending);
    void reqRefetch(uint64_t viewGen, uint64_t copyRequestId, const dtv::core::RefetchKey &key);
    void reqShutdown();

    void reqCount(uint64_t viewGen, const QString &path, const QString &tableName);
    void reqCountShutdown();

private:
    std::unique_ptr<QTemporaryDir> m_tempDir;
    QString m_dbPath;
    QString m_bigDbPath;

    bool createStandardDb(const QString &filePath, int rowCount);
    bool createMultiMillionDb(const QString &filePath, int rowCount);
    QString createCsvFile(const QString &fileName, int rowCount);
};

bool TestSourceWorker::createStandardDb(const QString &filePath, int rowCount)
{
    sqlite3 *db = nullptr;
    if(sqlite3_open(filePath.toUtf8().constData(), &db) != SQLITE_OK) {
        return false;
    }
    // default journal mode
    sqlite3_exec(db, "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, score REAL);", nullptr,
                 nullptr, nullptr);
    sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO items (id, name, score) VALUES (?1, ?2, ?3);", -1, &stmt,
                       nullptr);
    for(int i = 1; i <= rowCount; ++i) {
        sqlite3_bind_int64(stmt, 1, i);
        std::string name = "Item_" + std::to_string(i);
        sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 3, i * 1.5);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return true;
}

bool TestSourceWorker::createMultiMillionDb(const QString &filePath, int rowCount)
{
    sqlite3 *db = nullptr;
    if(sqlite3_open(filePath.toUtf8().constData(), &db) != SQLITE_OK) {
        return false;
    }
    sqlite3_exec(db, "PRAGMA journal_mode = OFF; PRAGMA synchronous = OFF;", nullptr, nullptr,
                 nullptr);
    sqlite3_exec(db, "CREATE TABLE big_items (id INTEGER PRIMARY KEY, name TEXT, score REAL);",
                 nullptr, nullptr, nullptr);
    std::string sql = "WITH RECURSIVE cnt(x) AS ("
                      "  SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < " +
                      std::to_string(rowCount) +
                      ") "
                      "INSERT INTO big_items(id, name, score) "
                      "SELECT x, 'Item_' || x, (x * 1.5) FROM cnt;";
    char *errmsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errmsg);
    if(errmsg) {
        sqlite3_free(errmsg);
    }
    sqlite3_close(db);
    return rc == SQLITE_OK;
}

void TestSourceWorker::initTestCase()
{
    registerWorkerMetatypes();
    m_tempDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tempDir->isValid());
    m_dbPath = m_tempDir->filePath("test.db");
    m_bigDbPath = m_tempDir->filePath("big.db");

    QVERIFY(createStandardDb(m_dbPath, 1500));
    QVERIFY(createMultiMillionDb(m_bigDbPath, 2000000));
}

void TestSourceWorker::cleanupTestCase()
{
    m_tempDir.reset();
}

void TestSourceWorker::testStartupAndOpen()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    BackgroundThread thread;
    SourceWorker worker(viewGen, opGen);
    worker.moveToThread(&thread);

    connect(this, &TestSourceWorker::reqOpen, &worker, &SourceWorker::open);
    QSignalSpy spyOpen(&worker, &SourceWorker::openCompleted);

    thread.start();

    emit reqOpen(1, 1, m_dbPath, "items");
    QVERIFY(spyOpen.wait(5000));
    QCOMPARE(spyOpen.count(), 1);

    auto args = spyOpen.takeFirst();
    QCOMPARE(args.at(0).toULongLong(), 1ULL);
    QCOMPARE(args.at(1).toULongLong(), 1ULL);
    QCOMPARE(args.at(2).toBool(), true);
    QVERIFY(args.at(3).toString().isEmpty());

    auto columns = args.at(4).value<std::vector<core::ColumnMeta>>();
    QCOMPARE(columns.size(), 3ULL);
    QCOMPARE(columns[0].name, std::string("id"));
    QCOMPARE(columns[1].name, std::string("name"));
    QCOMPARE(columns[2].name, std::string("score"));
    QCOMPARE(args.at(5).toBool(), true);

    thread.quit();
    thread.wait();
}

void TestSourceWorker::testFirstPage()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    BackgroundThread thread;
    SourceWorker worker(viewGen, opGen);
    worker.moveToThread(&thread);

    connect(this, &TestSourceWorker::reqOpen, &worker, &SourceWorker::open);
    connect(this, &TestSourceWorker::reqFirst, &worker, &SourceWorker::first);
    QSignalSpy spyOpen(&worker, &SourceWorker::openCompleted);
    QSignalSpy spyPage(&worker, &SourceWorker::pageReady);

    thread.start();

    emit reqOpen(1, 1, m_dbPath, "items");
    QVERIFY(spyOpen.wait(5000));

    emit reqFirst(1, 1, 500);
    QVERIFY(spyPage.wait(5000));
    QCOMPARE(spyPage.count(), 1);

    auto args = spyPage.takeFirst();
    QCOMPARE(args.at(0).toULongLong(), 1ULL);
    QCOMPARE(args.at(1).toULongLong(), 1ULL);

    auto result = args.at(2).value<std::shared_ptr<const core::PageResult>>();
    QVERIFY(result != nullptr);
    QVERIFY(result->ok);
    QVERIFY(result->data != nullptr);
    QCOMPARE(result->data->rows.size(), 500ULL);
    QCOMPARE(result->hasMore, true);
    QCOMPARE(result->token.firstKey, 1LL);
    QCOMPARE(result->token.lastKey, 500LL);

    thread.quit();
    thread.wait();
}

void TestSourceWorker::testNextPage()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    BackgroundThread thread;
    SourceWorker worker(viewGen, opGen);
    worker.moveToThread(&thread);

    connect(this, &TestSourceWorker::reqOpen, &worker, &SourceWorker::open);
    connect(this, &TestSourceWorker::reqFirst, &worker, &SourceWorker::first);
    connect(this, &TestSourceWorker::reqNext, &worker, &SourceWorker::next);
    QSignalSpy spyOpen(&worker, &SourceWorker::openCompleted);
    QSignalSpy spyPage(&worker, &SourceWorker::pageReady);

    thread.start();

    emit reqOpen(1, 1, m_dbPath, "items");
    QVERIFY(spyOpen.wait(5000));

    emit reqFirst(1, 1, 500);
    QVERIFY(spyPage.wait(5000));
    auto page1 = spyPage.takeFirst().at(2).value<std::shared_ptr<const core::PageResult>>();

    emit reqNext(1, 1, page1->token, 500);
    QVERIFY(spyPage.wait(5000));
    auto page2 = spyPage.takeFirst().at(2).value<std::shared_ptr<const core::PageResult>>();

    QVERIFY(page2->ok);
    QCOMPARE(page2->data->rows.size(), 500ULL);
    QCOMPARE(page2->hasMore, true);
    QCOMPARE(page2->token.firstKey, 501LL);
    QCOMPARE(page2->token.lastKey, 1000LL);
    QCOMPARE(page2->data->rows.front().front(), std::string("501"));

    thread.quit();
    thread.wait();
}

void TestSourceWorker::testPrevPage()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    BackgroundThread thread;
    SourceWorker worker(viewGen, opGen);
    worker.moveToThread(&thread);

    connect(this, &TestSourceWorker::reqOpen, &worker, &SourceWorker::open);
    connect(this, &TestSourceWorker::reqFirst, &worker, &SourceWorker::first);
    connect(this, &TestSourceWorker::reqNext, &worker, &SourceWorker::next);
    connect(this, &TestSourceWorker::reqPrev, &worker, &SourceWorker::prev);
    QSignalSpy spyOpen(&worker, &SourceWorker::openCompleted);
    QSignalSpy spyPage(&worker, &SourceWorker::pageReady);

    thread.start();

    emit reqOpen(1, 1, m_dbPath, "items");
    QVERIFY(spyOpen.wait(5000));

    emit reqFirst(1, 1, 500);
    QVERIFY(spyPage.wait(5000));
    auto page1 = spyPage.takeFirst().at(2).value<std::shared_ptr<const core::PageResult>>();

    emit reqNext(1, 1, page1->token, 500);
    QVERIFY(spyPage.wait(5000));
    auto page2 = spyPage.takeFirst().at(2).value<std::shared_ptr<const core::PageResult>>();

    emit reqPrev(1, 1, page2->token, 500);
    QVERIFY(spyPage.wait(5000));
    auto prevPage = spyPage.takeFirst().at(2).value<std::shared_ptr<const core::PageResult>>();

    QVERIFY(prevPage->ok);
    QCOMPARE(prevPage->data->rows.size(), 500ULL);
    QCOMPARE(prevPage->token.firstKey, 1LL);
    QCOMPARE(prevPage->token.lastKey, 500LL);

    thread.quit();
    thread.wait();
}

void TestSourceWorker::testLastPage()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    BackgroundThread thread;
    SourceWorker worker(viewGen, opGen);
    worker.moveToThread(&thread);

    connect(this, &TestSourceWorker::reqOpen, &worker, &SourceWorker::open);
    connect(this, &TestSourceWorker::reqLast, &worker, &SourceWorker::last);
    QSignalSpy spyOpen(&worker, &SourceWorker::openCompleted);
    QSignalSpy spyPage(&worker, &SourceWorker::pageReady);

    thread.start();

    emit reqOpen(1, 1, m_dbPath, "items");
    QVERIFY(spyOpen.wait(5000));

    emit reqLast(1, 1, 500, 1500);
    QVERIFY(spyPage.wait(5000));
    auto lastPage = spyPage.takeFirst().at(2).value<std::shared_ptr<const core::PageResult>>();

    QVERIFY(lastPage->ok);
    QCOMPARE(lastPage->data->rows.size(), 500ULL);
    QCOMPARE(lastPage->hasMore, false);
    QCOMPARE(lastPage->token.firstKey, 1001LL);
    QCOMPARE(lastPage->token.lastKey, 1500LL);
    QCOMPARE(lastPage->data->rows.back().front(), std::string("1500"));

    thread.quit();
    thread.wait();
}

void TestSourceWorker::testStaleQueuedRequestDiscarded()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    BackgroundThread thread;
    SourceWorker worker(viewGen, opGen);
    worker.moveToThread(&thread);

    connect(this, &TestSourceWorker::reqOpen, &worker, &SourceWorker::open);
    connect(this, &TestSourceWorker::reqFirst, &worker, &SourceWorker::first);
    QSignalSpy spyOpen(&worker, &SourceWorker::openCompleted);
    QSignalSpy spyPage(&worker, &SourceWorker::pageReady);

    thread.start();

    emit reqOpen(1, 1, m_dbPath, "items");
    QVERIFY(spyOpen.wait(5000));

    // Request with opGen 1
    emit reqFirst(1, 1, 500);
    // Bump opGen to 2 immediately
    opGen->store(2);
    // Request with opGen 2
    emit reqFirst(1, 2, 500);

    QVERIFY(spyPage.wait(5000));
    QTest::qWait(50);

    // Only the opGen 2 request should produce a result
    QCOMPARE(spyPage.count(), 1);
    auto args = spyPage.takeFirst();
    QCOMPARE(args.at(0).toULongLong(), 1ULL);
    QCOMPARE(args.at(1).toULongLong(), 2ULL);

    thread.quit();
    thread.wait();
}

void TestSourceWorker::testViewGenCancellation()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    BackgroundThread thread;
    SourceWorker worker(viewGen, opGen);
    worker.moveToThread(&thread);

    connect(this, &TestSourceWorker::reqOpen, &worker, &SourceWorker::open);
    connect(this, &TestSourceWorker::reqFirst, &worker, &SourceWorker::first);
    QSignalSpy spyOpen(&worker, &SourceWorker::openCompleted);
    QSignalSpy spyPage(&worker, &SourceWorker::pageReady);

    thread.start();

    emit reqOpen(1, 1, m_dbPath, "items");
    QVERIFY(spyOpen.wait(5000));

    // Invalidate viewGen
    viewGen->store(2);

    // Request with stale viewGen 1
    emit reqFirst(1, 1, 500);

    // Must be dropped
    QTest::qWait(100);
    QCOMPARE(spyPage.count(), 0);

    thread.quit();
    thread.wait();
}

void TestSourceWorker::testOpGenCancellation()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    BackgroundThread threadSource;
    BackgroundThread threadCount;

    SourceWorker sourceWorker(viewGen, opGen);
    CountWorker countWorker(viewGen);

    sourceWorker.moveToThread(&threadSource);
    countWorker.moveToThread(&threadCount);

    connect(this, &TestSourceWorker::reqOpen, &sourceWorker, &SourceWorker::open);
    connect(this, &TestSourceWorker::reqFirst, &sourceWorker, &SourceWorker::first);
    connect(this, &TestSourceWorker::reqCount, &countWorker, &CountWorker::count);

    QSignalSpy spyOpen(&sourceWorker, &SourceWorker::openCompleted);
    QSignalSpy spyPage(&sourceWorker, &SourceWorker::pageReady);
    QSignalSpy spyCount(&countWorker, &CountWorker::countCompleted);

    threadSource.start();
    threadCount.start();

    emit reqOpen(1, 1, m_dbPath, "items");
    QVERIFY(spyOpen.wait(5000));

    // Start COUNT with viewGen 1
    emit reqCount(1, m_dbPath, "items");

    // Enqueue page fetch with opGen 1, then immediately bump opGen
    emit reqFirst(1, 1, 500);
    opGen->store(2);

    // SourceWorker with opGen 1 should be dropped
    // CountWorker depends only on viewGen, so COUNT must succeed!
    QVERIFY(spyCount.wait(5000));
    QCOMPARE(spyCount.count(), 1);
    auto countArgs = spyCount.takeFirst();
    QCOMPARE(countArgs.at(0).toULongLong(), 1ULL);
    QCOMPARE(countArgs.at(1).toBool(), true);
    QCOMPARE(countArgs.at(3).toLongLong(), 1500LL);

    QTest::qWait(50);
    QCOMPARE(spyPage.count(), 0);

    threadSource.quit();
    threadSource.wait();
    threadCount.quit();
    threadCount.wait();
}

void TestSourceWorker::testCountProceedingWhileNavigationContinues()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    BackgroundThread threadSource;
    BackgroundThread threadCount;

    SourceWorker sourceWorker(viewGen, opGen);
    CountWorker countWorker(viewGen);

    sourceWorker.moveToThread(&threadSource);
    countWorker.moveToThread(&threadCount);

    connect(this, &TestSourceWorker::reqOpen, &sourceWorker, &SourceWorker::open);
    connect(this, &TestSourceWorker::reqFirst, &sourceWorker, &SourceWorker::first);
    connect(this, &TestSourceWorker::reqCount, &countWorker, &CountWorker::count);

    QSignalSpy spyOpen(&sourceWorker, &SourceWorker::openCompleted);
    QSignalSpy spyPage(&sourceWorker, &SourceWorker::pageReady);
    QSignalSpy spyCount(&countWorker, &CountWorker::countCompleted);

    threadSource.start();
    threadCount.start();

    emit reqOpen(1, 1, m_bigDbPath, "big_items");
    QVERIFY(spyOpen.wait(5000));

    // Start long count on 2M rows
    emit reqCount(1, m_bigDbPath, "big_items");

    // While count is running, fetch first page on source worker
    emit reqFirst(1, 1, 500);

    // Page must arrive promptly without waiting for COUNT
    QVERIFY(spyPage.wait(2000));
    QCOMPARE(spyPage.count(), 1);
    auto page = spyPage.takeFirst().at(2).value<std::shared_ptr<const core::PageResult>>();
    QVERIFY(page->ok);
    QCOMPARE(page->data->rows.size(), 500ULL);

    // Then count arrives
    QVERIFY(spyCount.wait(5000));
    QCOMPARE(spyCount.count(), 1);
    auto countArgs = spyCount.takeFirst();
    QCOMPARE(countArgs.at(1).toBool(), true);
    QCOMPARE(countArgs.at(3).toLongLong(), 2000000LL);

    threadSource.quit();
    threadSource.wait();
    threadCount.quit();
    threadCount.wait();
}

void TestSourceWorker::testCountCancellationAndLatency()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);

    BackgroundThread thread;
    CountWorker worker(viewGen);
    worker.moveToThread(&thread);

    connect(this, &TestSourceWorker::reqCount, &worker, &CountWorker::count);
    QSignalSpy spyCount(&worker, &CountWorker::countCompleted);

    std::chrono::steady_clock::time_point interruptTime;
    std::chrono::steady_clock::time_point finishTime;
    std::atomic<bool> interrupted{false};
    std::atomic<bool> finished{false};

    worker.setInterruptHook([&]() {
        interrupted = true;
        interruptTime = std::chrono::steady_clock::now();
        worker.interrupt();
        viewGen->fetch_add(1);
    });

    worker.setFinishHook([&]() {
        finishTime = std::chrono::steady_clock::now();
        finished = true;
    });

    thread.start();

    emit reqCount(1, m_bigDbPath, "big_items");

    // Wait for worker operation to finish
    QTRY_VERIFY_WITH_TIMEOUT(finished.load(), 5000);
    QVERIFY(interrupted.load());

    auto latencyMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(finishTime - interruptTime).count();
    qInfo("Measured CountWorker cancellation latency: %lld ms", static_cast<long long>(latencyMs));

    // Interruption must settle promptly (< 100 ms)
    QVERIFY(latencyMs < 100);

    // Stale generation count signal was dropped
    QCOMPARE(spyCount.count(), 0);

    thread.quit();
    thread.wait();
}

void TestSourceWorker::testSortCancellation()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    BackgroundThread thread;
    SourceWorker worker(viewGen, opGen);
    worker.moveToThread(&thread);

    connect(this, &TestSourceWorker::reqOpen, &worker, &SourceWorker::open);
    connect(this, &TestSourceWorker::reqSort, &worker, &SourceWorker::sort);

    QSignalSpy spyOpen(&worker, &SourceWorker::openCompleted);
    QSignalSpy spySort(&worker, &SourceWorker::sortCompleted);

    std::chrono::steady_clock::time_point interruptTime;
    std::chrono::steady_clock::time_point finishTime;
    std::atomic<bool> interrupted{false};
    std::atomic<bool> finished{false};

    thread.start();

    emit reqOpen(1, 1, m_bigDbPath, "big_items");
    QVERIFY(spyOpen.wait(5000));

    worker.setProgressHook([&]() {
        if(!interrupted.exchange(true)) {
            interruptTime = std::chrono::steady_clock::now();
            worker.interrupt();
            opGen->fetch_add(1);
        }
    });

    worker.setFinishHook([&]() {
        if(interrupted.load()) {
            finishTime = std::chrono::steady_clock::now();
            finished = true;
        }
    });

    // Sort column 1 (name TEXT) descending on 2M rows
    emit reqSort(1, 1, 1, false);

    QTRY_VERIFY_WITH_TIMEOUT(finished.load(), 5000);
    QVERIFY(interrupted.load());

    auto latencyMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(finishTime - interruptTime).count();
    qInfo("Measured sort cancellation latency: %lld ms", static_cast<long long>(latencyMs));
    QVERIFY(latencyMs < 2000);

    // Stale generation signal dropped
    QCOMPARE(spySort.count(), 0);

    thread.quit();
    thread.wait();
}

void TestSourceWorker::testInterruptRetryWhenCurrent()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    BackgroundThread thread;
    SourceWorker worker(viewGen, opGen);
    worker.moveToThread(&thread);

    connect(this, &TestSourceWorker::reqOpen, &worker, &SourceWorker::open);
    connect(this, &TestSourceWorker::reqFirst, &worker, &SourceWorker::first);

    QSignalSpy spyOpen(&worker, &SourceWorker::openCompleted);
    QSignalSpy spyPage(&worker, &SourceWorker::pageReady);

    thread.start();

    emit reqOpen(1, 1, m_dbPath, "items");
    QVERIFY(spyOpen.wait(5000));

    // Interrupt once on connection before request starts
    worker.interrupt();

    // Now execute first with CURRENT generation 1, 1
    // The worker receiving interrupt while current should retry once and succeed!
    emit reqFirst(1, 1, 500);

    QVERIFY(spyPage.wait(5000));
    QCOMPARE(spyPage.count(), 1);
    auto res = spyPage.takeFirst().at(2).value<std::shared_ptr<const core::PageResult>>();
    QVERIFY(res->ok);
    QCOMPARE(res->data->rows.size(), 500ULL);

    thread.quit();
    thread.wait();
}

void TestSourceWorker::testCancelInFlightWithoutGenBumpDoesNotRetry()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    BackgroundThread thread;
    SourceWorker worker(viewGen, opGen);
    worker.moveToThread(&thread);

    connect(this, &TestSourceWorker::reqOpen, &worker, &SourceWorker::open);
    connect(this, &TestSourceWorker::reqSort, &worker, &SourceWorker::sort);
    QSignalSpy spyOpen(&worker, &SourceWorker::openCompleted);
    QSignalSpy spySort(&worker, &SourceWorker::sortCompleted);

    thread.start();

    emit reqOpen(1, 1, m_bigDbPath, "big_items");
    QVERIFY(spyOpen.wait(5000));

    std::atomic<bool> interrupted{false};
    std::atomic<int> interruptCalls{0};
    std::atomic<bool> finished{false};

    worker.setProgressHook([&]() {
        if(!interrupted.exchange(true)) {
            interruptCalls.fetch_add(1);
            worker.interrupt();
        }
    });

    worker.setFinishHook([&]() {
        finished = true;
    });

    emit reqSort(1, 1, 1, false);

    QTRY_VERIFY_WITH_TIMEOUT(finished.load(), 5000);

    QCOMPARE(interruptCalls.load(), 1);

    if(spySort.isEmpty()) {
        QVERIFY(spySort.wait(1000));
    }
    QCOMPARE(spySort.count(), 1);
    auto args = spySort.takeFirst();
    QCOMPARE(args.at(2).toBool(), false); // ok == false

    thread.quit();
    thread.wait();
}

void TestSourceWorker::testOpenPreservesCancelCheckAndAborts()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    BackgroundThread thread;
    SourceWorker worker(viewGen, opGen);

    auto sourceHandle = worker.interruptHandle();
    QVERIFY(sourceHandle != nullptr);

    worker.moveToThread(&thread);

    connect(this, &TestSourceWorker::reqOpen, &worker, &SourceWorker::open);
    QSignalSpy spyOpen(&worker, &SourceWorker::openCompleted);

    viewGen->store(2);

    thread.start();

    emit reqOpen(1, 1, m_dbPath, "items");

    QVERIFY(!spyOpen.wait(1000));

    thread.quit();
    thread.wait();
}

void TestSourceWorker::testAbortedOpenClosesDbAndReleasesLock()
{
    QString lockDbPath = m_tempDir->filePath("lock_test.db");
    QVERIFY(createStandardDb(lockDbPath, 10));

    // Case 1: Open with non-existent table fails at metadata query
    {
        parsers::SqliteTableSource source;
        auto handle = source.interruptHandle();
        QVERIFY(handle != nullptr);

        bool ok = source.open(lockDbPath.toStdString(), "non_existent_table");
        QVERIFY(!ok);
        QCOMPARE(handle->db(), static_cast<sqlite3 *>(nullptr));

        // On Windows, if db handle was leaked, QFile::remove would fail due to file lock
        QVERIFY(QFile::remove(lockDbPath));
    }

    // Re-create db for Case 2
    QVERIFY(createStandardDb(lockDbPath, 10));

    // Case 2: Open aborted by cancellation
    {
        parsers::SqliteTableSource source;
        auto handle = source.interruptHandle();
        QVERIFY(handle != nullptr);

        source.setCancelCheck([]() {
            return true;
        });

        bool ok = source.open(lockDbPath.toStdString(), "items");
        QVERIFY(!ok);
        QCOMPARE(source.error(), std::string("Cancelled"));
        QCOMPARE(handle->db(), static_cast<sqlite3 *>(nullptr));

        QVERIFY(QFile::remove(lockDbPath));
    }
}

void TestSourceWorker::testShutdownAndConnectionCleanup()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    auto threadSource = std::make_unique<BackgroundThread>();
    auto threadCount = std::make_unique<BackgroundThread>();

    auto *sourceWorker = new SourceWorker(viewGen, opGen);
    auto *countWorker = new CountWorker(viewGen);

    auto sourceHandle = sourceWorker->interruptHandle();
    auto countHandle = countWorker->interruptHandle();

    QVERIFY(sourceHandle != nullptr);
    QVERIFY(countHandle != nullptr);

    sourceWorker->moveToThread(threadSource.get());
    countWorker->moveToThread(threadCount.get());

    threadSource->start();
    threadCount->start();

    // Open connection
    QSignalSpy spyOpen(sourceWorker, &SourceWorker::openCompleted);
    QMetaObject::invokeMethod(sourceWorker, "open", Qt::QueuedConnection, Q_ARG(uint64_t, 1),
                              Q_ARG(uint64_t, 1), Q_ARG(QString, m_dbPath),
                              Q_ARG(QString, "items"));
    QVERIFY(spyOpen.wait(5000));
    QVERIFY(sourceHandle->db() != nullptr);

    QSignalSpy spySourceDestroyed(sourceWorker, &QObject::destroyed);
    QSignalSpy spyCountDestroyed(countWorker, &QObject::destroyed);

    // Call shutdown slots
    QMetaObject::invokeMethod(sourceWorker, "shutdown", Qt::QueuedConnection);
    QMetaObject::invokeMethod(countWorker, "shutdown", Qt::QueuedConnection);

    if(spySourceDestroyed.isEmpty()) {
        QVERIFY(spySourceDestroyed.wait(5000));
    }
    QCOMPARE(spySourceDestroyed.count(), 1);

    if(spyCountDestroyed.isEmpty()) {
        QVERIFY(spyCountDestroyed.wait(5000));
    }
    QCOMPARE(spyCountDestroyed.count(), 1);

    // Threads quit and wait (destructors call quit()+wait())
    threadSource.reset();
    threadCount.reset();

    // After shutdown and destruction, connection handles must be cleared
    QCOMPARE(sourceHandle->db(), static_cast<sqlite3 *>(nullptr));
    QCOMPARE(countHandle->db(), static_cast<sqlite3 *>(nullptr));
}

QString TestSourceWorker::createCsvFile(const QString &fileName, int rowCount)
{
    QString path = m_tempDir->filePath(fileName);
    QFile file(path);
    if(!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return QString();
    }
    QTextStream out(&file);
    out << "id,name,score\n";
    for(int i = 1; i <= rowCount; ++i) {
        out << i << ",Item_" << i << "," << (i * 1.5) << "\n";
    }
    file.close();
    return path;
}

void TestSourceWorker::testCsvOpenAndFirstPage()
{
    QString csvPath = createCsvFile("first_page.csv", 3000);
    QVERIFY(!csvPath.isEmpty());

    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    auto thread = std::make_unique<BackgroundThread>();
    auto *worker = new SourceWorker(viewGen, opGen);
    worker->moveToThread(thread.get());
    thread->start();

    QSignalSpy spyOpen(worker, &SourceWorker::openCompleted);
    QSignalSpy spyPage(worker, &SourceWorker::pageReady);
    QSignalSpy spyProgress(worker, &SourceWorker::indexProgress);

    SourceOpenDescriptor desc{SourceKind::Csv, csvPath, "", ','};
    QMetaObject::invokeMethod(worker, "openDescriptor", Qt::QueuedConnection, Q_ARG(uint64_t, 1),
                              Q_ARG(uint64_t, 1), Q_ARG(dtv::workers::SourceOpenDescriptor, desc));

    QVERIFY(spyOpen.wait(5000));
    QCOMPARE(spyOpen.count(), 1);
    QCOMPARE(spyOpen.at(0).at(2).toBool(), true); // ok == true

    // Request first page
    QMetaObject::invokeMethod(worker, "first", Qt::QueuedConnection, Q_ARG(uint64_t, 1),
                              Q_ARG(uint64_t, 1), Q_ARG(int, 500));

    QVERIFY(spyPage.wait(2000));
    QCOMPARE(spyPage.count(), 1);
    auto pageRes = spyPage.at(0).at(2).value<std::shared_ptr<const dtv::core::PageResult>>();
    QVERIFY(pageRes->ok);
    QCOMPARE(pageRes->data->rows.size(), 500ull);
    QCOMPARE(pageRes->token.offset, 0LL);

    // Wait for indexing to complete
    bool completed = false;
    for(int attempt = 0; attempt < 50 && !completed; ++attempt) {
        if(!spyProgress.isEmpty()) {
            for(const auto &sig : spyProgress) {
                if(sig.at(2).toBool()) { // isComplete
                    completed = true;
                    break;
                }
            }
        }
        if(!completed) {
            spyProgress.wait(100);
        }
    }
    QVERIFY(completed);

    QMetaObject::invokeMethod(worker, "shutdown", Qt::QueuedConnection);
    thread->quit();
    thread->wait();
}

void TestSourceWorker::testCsvPendingPageWaitAndFulfillment()
{
    // 15,000 rows requires multiple 256 KiB slices
    QString csvPath = createCsvFile("pending_page.csv", 15000);
    QVERIFY(!csvPath.isEmpty());

    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    auto thread = std::make_unique<BackgroundThread>();
    auto *worker = new SourceWorker(viewGen, opGen);
    worker->moveToThread(thread.get());
    thread->start();

    QSignalSpy spyOpen(worker, &SourceWorker::openCompleted);
    QSignalSpy spyPage(worker, &SourceWorker::pageReady);

    SourceOpenDescriptor desc{SourceKind::Csv, csvPath, "", ','};
    QMetaObject::invokeMethod(worker, "openDescriptor", Qt::QueuedConnection, Q_ARG(uint64_t, 1),
                              Q_ARG(uint64_t, 1), Q_ARG(dtv::workers::SourceOpenDescriptor, desc));

    QVERIFY(spyOpen.wait(5000));

    // Request a page far beyond the initial slice (e.g. ordinal 12,000)
    dtv::core::PageToken token;
    token.offset = 11500;
    token.valid = true;

    opGen->store(2);
    QMetaObject::invokeMethod(worker, "next", Qt::QueuedConnection, Q_ARG(uint64_t, 1),
                              Q_ARG(uint64_t, 2), Q_ARG(dtv::core::PageToken, token),
                              Q_ARG(int, 500));

    // Must wait for background indexing slices to reach ordinal 12,000
    QVERIFY(spyPage.wait(5000));
    QCOMPARE(spyPage.count(), 1);
    auto pageRes = spyPage.at(0).at(2).value<std::shared_ptr<const dtv::core::PageResult>>();
    QVERIFY(pageRes->ok);
    QCOMPARE(pageRes->token.offset, 12000LL);
    QCOMPARE(pageRes->data->rows.size(), 500ull);

    QMetaObject::invokeMethod(worker, "shutdown", Qt::QueuedConnection);
    thread->quit();
    thread->wait();
}

void TestSourceWorker::testCsvOpGenSupersedesPendingRequestLeavesIndexingAlive()
{
    // 60,000 rows (approx 1.4 MiB) ensures both requests are far beyond the initial 256 KiB slice (~11,669 rows)
    QString csvPath = createCsvFile("supersede.csv", 60000);
    QVERIFY(!csvPath.isEmpty());

    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    auto thread = std::make_unique<BackgroundThread>();
    auto *worker = new SourceWorker(viewGen, opGen);
    worker->moveToThread(thread.get());
    thread->start();

    QSignalSpy spyOpen(worker, &SourceWorker::openCompleted);
    QSignalSpy spyPage(worker, &SourceWorker::pageReady);
    QSignalSpy spyProgress(worker, &SourceWorker::indexProgress);

    SourceOpenDescriptor desc{SourceKind::Csv, csvPath, "", ','};
    QMetaObject::invokeMethod(worker, "openDescriptor", Qt::QueuedConnection, Q_ARG(uint64_t, 1),
                              Q_ARG(uint64_t, 1), Q_ARG(dtv::workers::SourceOpenDescriptor, desc));

    QVERIFY(spyOpen.wait(5000));

    // Request A targets 25,000 (offset 24500)
    dtv::core::PageToken tokenA;
    tokenA.offset = 24500;
    tokenA.valid = true;

    // Request B targets 35,000 (offset 34500)
    dtv::core::PageToken tokenB;
    tokenB.offset = 34500;
    tokenB.valid = true;

    // Issue request A with opGen 2
    opGen->store(2);
    QMetaObject::invokeMethod(worker, "next", Qt::QueuedConnection, Q_ARG(uint64_t, 1),
                              Q_ARG(uint64_t, 2), Q_ARG(dtv::core::PageToken, tokenA),
                              Q_ARG(int, 500));

    // Immediately supersede with request B with opGen 3
    opGen->store(3);
    QMetaObject::invokeMethod(worker, "next", Qt::QueuedConnection, Q_ARG(uint64_t, 1),
                              Q_ARG(uint64_t, 3), Q_ARG(dtv::core::PageToken, tokenB),
                              Q_ARG(int, 500));

    // Neither request should have arrived immediately (both genuinely pending)
    QCOMPARE(spyPage.count(), 0);

    // Only request B (opGen 3) should arrive once indexing reaches ordinal 35,000
    QVERIFY(spyPage.wait(5000));
    QCOMPARE(spyPage.count(), 1);
    QCOMPARE(spyPage.at(0).at(1).toULongLong(), 3ULL); // opGen == 3
    auto pageRes = spyPage.at(0).at(2).value<std::shared_ptr<const dtv::core::PageResult>>();
    QCOMPARE(pageRes->token.offset, 35000LL);

    // Verify background indexing still reaches EOF despite opGen bump
    bool completed = false;
    for(int attempt = 0; attempt < 50 && !completed; ++attempt) {
        if(!spyProgress.isEmpty()) {
            for(const auto &sig : spyProgress) {
                if(sig.at(2).toBool()) {
                    completed = true;
                    break;
                }
            }
        }
        if(!completed) {
            spyProgress.wait(100);
        }
    }
    QVERIFY(completed);

    QMetaObject::invokeMethod(worker, "shutdown", Qt::QueuedConnection);
    thread->quit();
    thread->wait();
}

void TestSourceWorker::testCsvViewGenCancellationStopsIndexing()
{
    // 60,000 rows (multi-slice)
    QString csvPath = createCsvFile("cancel_indexing.csv", 60000);
    QVERIFY(!csvPath.isEmpty());

    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    auto thread = std::make_unique<BackgroundThread>();
    auto *worker = new SourceWorker(viewGen, opGen);
    worker->moveToThread(thread.get());
    thread->start();

    QSignalSpy spyOpen(worker, &SourceWorker::openCompleted);
    QSignalSpy spyProgress(worker, &SourceWorker::indexProgress);

    SourceOpenDescriptor desc{SourceKind::Csv, csvPath, "", ','};
    QMetaObject::invokeMethod(worker, "openDescriptor", Qt::QueuedConnection, Q_ARG(uint64_t, 1),
                              Q_ARG(uint64_t, 1), Q_ARG(dtv::workers::SourceOpenDescriptor, desc));

    QVERIFY(spyOpen.wait(5000));

    // Bump viewGen while indexing is actively running
    viewGen->store(2);

    // Wait a brief moment to let slice stop
    QTest::qWait(150);

    // Stopping means no further slice runs, so the progress count freezes.
    const int frozenProgress = spyProgress.count();
    QTest::qWait(250);
    QCOMPARE(spyProgress.count(), frozenProgress);

    // Verify indexing never emitted isComplete for viewGen 1
    for(const auto &sig : spyProgress) {
        if(sig.at(0).toULongLong() == 1ULL) {
            QVERIFY(!sig.at(2).toBool()); // isComplete must be false
        }
    }

    QMetaObject::invokeMethod(worker, "shutdown", Qt::QueuedConnection);
    thread->quit();
    thread->wait();
}

namespace {
class MockBudgetTableSource : public dtv::core::ITableSource {
public:
    const std::vector<dtv::core::ColumnMeta> &columns() const override
    {
        return m_cols;
    }
    std::optional<int64_t> rowCount() const override
    {
        return 10;
    }
    void setKnownTotal(int64_t) override
    {}
    dtv::core::PageResult first(int) override
    {
        return {};
    }
    dtv::core::PageResult next(const dtv::core::PageToken &, int) override
    {
        return {};
    }
    dtv::core::PageResult prev(const dtv::core::PageToken &, int) override
    {
        return {};
    }
    dtv::core::PageResult last(int, std::optional<int64_t>) override
    {
        return {};
    }
    bool canSort() const override
    {
        return false;
    }
    bool canRefetch() const override
    {
        return true;
    }
    bool sort(size_t, bool, dtv::core::CancelCheck = {}) override
    {
        return false;
    }
    dtv::core::RefetchResult refetch(const dtv::core::RefetchKey &) override
    {
        dtv::core::RefetchResult res;
        res.ok = true;
        res.columns = {0};
        res.values = {std::string(40 * 1024 * 1024, 'X')}; // 40 MiB per row
        return res;
    }

private:
    std::vector<dtv::core::ColumnMeta> m_cols{{"col", dtv::core::ColumnMeta::Type::String}};
};
} // namespace

void TestSourceWorker::testRefetchRowsBatchBudget()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    auto thread = std::make_unique<BackgroundThread>();
    // Inject MockBudgetTableSource where each row returns 40 MiB
    auto *worker = new SourceWorker(viewGen, opGen, std::make_unique<MockBudgetTableSource>());
    worker->moveToThread(thread.get());
    thread->start();

    QSignalSpy spyRefetch(worker, &SourceWorker::refetchRowsCompleted);

    // Request 2 rows (2 * 40 MiB = 80 MiB > 64 MiB budget)
    std::vector<std::pair<int, dtv::core::RefetchKey>> rowKeys;
    dtv::core::RefetchKey key;
    key.rowid = 1;
    rowKeys.emplace_back(0, key);
    key.rowid = 2;
    rowKeys.emplace_back(1, key);

    QMetaObject::invokeMethod(worker, "refetchRows", Qt::QueuedConnection, Q_ARG(uint64_t, 1),
                              Q_ARG(uint64_t, 100), Q_ARG(RefetchRowKeysList, rowKeys));

    QVERIFY(spyRefetch.wait(5000));
    QCOMPARE(spyRefetch.count(), 1);
    auto results = spyRefetch.at(0).at(2).value<RefetchRowResultsList>();
    QCOMPARE(results.size(), 2ull);

    // Both rows must be invalidated once the budget is exceeded
    for(const auto &res : results) {
        QVERIFY(!res.second.ok);
        QCOMPARE(res.second.error, std::string("Copy budget exceeded (64 MiB)"));
        QVERIFY(res.second.values.empty());
    }

    QMetaObject::invokeMethod(worker, "shutdown", Qt::QueuedConnection);
    thread->quit();
    thread->wait();
}

void TestSourceWorker::testRefetchRowsCompletesAfterRetiredView()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    BackgroundThread thread;
    SourceWorker worker(viewGen, opGen);
    worker.moveToThread(&thread);

    connect(this, &TestSourceWorker::reqOpen, &worker, &SourceWorker::open);
    QSignalSpy spyRefetch(&worker, &SourceWorker::refetchRowsCompleted);
    QSignalSpy spyOpen(&worker, &SourceWorker::openCompleted);

    thread.start();

    emit reqOpen(1, 1, m_dbPath, "items");
    QVERIFY(spyOpen.wait(5000));

    // Retire the view the open above belongs to without touching the worker:
    // this is what a file switch does while a copy is still in flight.
    viewGen->store(2);

    // The renderer holds its cells and a copy counter for every request it
    // sent. Returning without a completion would leave that entry armed
    // forever, so both early exits have to report the failure themselves.
    std::vector<std::pair<int, dtv::core::RefetchKey>> rowKeys;
    dtv::core::RefetchKey key;
    key.rowid = 1;
    rowKeys.emplace_back(0, key);

    QMetaObject::invokeMethod(&worker, "refetchRows", Qt::QueuedConnection, Q_ARG(uint64_t, 1),
                              Q_ARG(uint64_t, 700), Q_ARG(RefetchRowKeysList, rowKeys));

    QVERIFY(spyRefetch.wait(5000));
    QCOMPARE(spyRefetch.count(), 1);
    QCOMPARE(spyRefetch.at(0).at(0).toULongLong(), 1ULL);
    QCOMPARE(spyRefetch.at(0).at(1).toULongLong(), 700ULL);
    QVERIFY(spyRefetch.at(0).at(2).value<RefetchRowResultsList>().empty());

    // No shutdown() call: it ends in deleteLater(), which must never reach a
    // worker that is not heap allocated.
    thread.quit();
    thread.wait();
}

void TestSourceWorker::testRefetchRowsCompletesAfterInterruption()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    BackgroundThread thread;
    SourceWorker worker(viewGen, opGen);
    worker.moveToThread(&thread);

    connect(this, &TestSourceWorker::reqOpen, &worker, &SourceWorker::open);
    QSignalSpy spyRefetch(&worker, &SourceWorker::refetchRowsCompleted);
    QSignalSpy spyOpen(&worker, &SourceWorker::openCompleted);

    thread.start();

    emit reqOpen(1, 1, m_dbPath, "items");
    QVERIFY(spyOpen.wait(5000));

    thread.requestInterruption();

    std::vector<std::pair<int, dtv::core::RefetchKey>> rowKeys;
    dtv::core::RefetchKey key;
    key.rowid = 1;
    rowKeys.emplace_back(0, key);

    QMetaObject::invokeMethod(&worker, "refetchRows", Qt::QueuedConnection, Q_ARG(uint64_t, 1),
                              Q_ARG(uint64_t, 800), Q_ARG(RefetchRowKeysList, rowKeys));

    QVERIFY(spyRefetch.wait(5000));
    QCOMPARE(spyRefetch.count(), 1);
    QCOMPARE(spyRefetch.at(0).at(1).toULongLong(), 800ULL);
    QVERIFY(spyRefetch.at(0).at(2).value<RefetchRowResultsList>().empty());

    // No shutdown() call: it ends in deleteLater(), which must never reach a
    // worker that is not heap allocated.
    thread.quit();
    thread.wait();
}

void TestSourceWorker::testSortFailureReportsReasonForNonSqliteSource()
{
    auto viewGen = std::make_shared<std::atomic<uint64_t>>(1);
    auto opGen = std::make_shared<std::atomic<uint64_t>>(1);

    auto thread = std::make_unique<BackgroundThread>();
    // A source that is not a SqliteTableSource: the worker must still deliver a
    // usable reason, otherwise the UI renders a bare "Sort failed: ".
    auto *worker = new SourceWorker(viewGen, opGen, std::make_unique<MockBudgetTableSource>());
    worker->moveToThread(thread.get());
    thread->start();

    QSignalSpy spySort(worker, &SourceWorker::sortCompleted);

    // Called through a functor: size_t has no natural Q_ARG spelling, and the
    // worker's sort slot takes it by value.
    QMetaObject::invokeMethod(
        worker,
        [worker] {
            worker->sort(1, 1, 0, true);
        },
        Qt::QueuedConnection);

    QVERIFY(spySort.wait(5000));
    QCOMPARE(spySort.count(), 1);
    QCOMPARE(spySort.at(0).at(2).toBool(), false);
    const QString reason = spySort.at(0).at(3).toString();
    QVERIFY(!reason.isEmpty());
    QCOMPARE(reason, QString("Sorting is not supported by this source"));

    QMetaObject::invokeMethod(worker, "shutdown", Qt::QueuedConnection);
    thread->quit();
    thread->wait();
}

QTEST_MAIN(TestSourceWorker)
#include "test_source_worker.moc"
