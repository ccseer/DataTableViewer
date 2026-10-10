#include <QtTest>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QClipboard>
#include <QGuiApplication>
#include <sqlite3.h>

#include "ui/data_table_viewer.h"
#include "ui/page_bar.h"
#include "ui/table_renderer.h"
#include "ui/table_picker.h"
#include "ui/table_model.h"
#include "ui/status_bar.h"
#include "ui/search_bar.h"
#include "ui/action_registry.h"
#include <QStackedLayout>
#include <QPushButton>
#include <QComboBox>
#include <QHeaderView>
#include <QTableView>
#include <QKeyEvent>
#include <QLabel>
#include <QFileInfo>
#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <QSignalSpy>
#include "workers/source_worker.h"

class TestViewerPaging : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    void test1MRowDatabaseOpensAndPages();
    void testSmallTableHidesPager();
    void testCsvPaged();
    void testServerSortAndCancel();
    void testRealHeaderClicksKeepCommittedIndicator();
    void testLateSortCancelAndReplacement();
    void testCopySurvivesPageTurnAndLatestWins();
    void testBackButtonTeardown();
    void testHeaderStatePreservedAcrossPageTurns();
    void testCountRequestedOnlyOnceOnPageTurns();
    void testPageNavigationFailureRollsBackPageNumber();
    void testLargeTableRowRangeDoesNotOverflow();
    void testClampedLongTextCopyRefetchesFullContent();
    void testEmptyPageNavigationPreservesStateAndToken();
    void testTextViewControlBarButton();
    void testRowIndexColumn();
    void testStatusBarRowIndexMetrics();
    void testPagingShortcutsAndTooltips();
    void testCancelPendingClearsForeignPagerState();
    void testContentSizingAndPropertyBounds();
    void testRepeatedIndexingErrorDoesNotAccumulate();
    void testViewerDestructionJoinsActiveWorkers();
    void testCopyBudgetDirectPathAccuracy();

private:
    std::unique_ptr<QTemporaryDir> m_tempDir;
    QString m_1mDbPath;
    QString m_smallDbPath;
    QString m_longTextDbPath;
    QString m_emptyPageDbPath;
    QString m_longTextValue;

    void setupViewer(DataTableViewer &viewer, const QString &path, ViewOptionsPrivate &optsPriv,
                     ViewOptions &opts);
    bool createDatabase(const QString &filePath, int rowCount);
    bool createLongTextDatabase(const QString &filePath, int textLength);
};

bool TestViewerPaging::createDatabase(const QString &filePath, int rowCount)
{
    sqlite3 *db = nullptr;
    if(sqlite3_open(filePath.toUtf8().constData(), &db) != SQLITE_OK) {
        return false;
    }
    sqlite3_exec(db, "PRAGMA journal_mode = OFF; PRAGMA synchronous = OFF;", nullptr, nullptr,
                 nullptr);
    sqlite3_exec(db, "CREATE TABLE items /* Inventory items table */ (id INTEGER PRIMARY KEY, name TEXT, val REAL);", nullptr,
                 nullptr, nullptr);
    std::string sql = "WITH RECURSIVE cnt(x) AS ("
                      "  SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < " +
                      std::to_string(rowCount) +
                      ") "
                      "INSERT INTO items(id, name, val) "
                      "SELECT x, 'Item_' || x, (x * 1.5) FROM cnt;";
    char *errmsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errmsg);
    if(errmsg) {
        sqlite3_free(errmsg);
    }
    sqlite3_close(db);
    return rc == SQLITE_OK;
}

bool TestViewerPaging::createLongTextDatabase(const QString &filePath, int textLength)
{
    sqlite3 *db = nullptr;
    if(sqlite3_open(filePath.toUtf8().constData(), &db) != SQLITE_OK) {
        return false;
    }
    sqlite3_exec(db, "PRAGMA journal_mode = OFF; PRAGMA synchronous = OFF;", nullptr, nullptr,
                 nullptr);
    sqlite3_exec(db, "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, val REAL);", nullptr,
                 nullptr, nullptr);
    std::string longText(static_cast<size_t>(textLength), 'A');
    std::string sql = "INSERT INTO items(id, name, val) VALUES (1, '" + longText + "', 1.5);";
    char *errmsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errmsg);
    if(errmsg) {
        sqlite3_free(errmsg);
    }
    sqlite3_close(db);
    return rc == SQLITE_OK;
}

void TestViewerPaging::setupViewer(DataTableViewer &viewer, const QString &path,
                                   ViewOptionsPrivate &optsPriv, ViewOptions &opts)
{
    // Keep the page size and the settings file inside the test's temp dir so
    // neither the host configuration nor the plugin directory is touched.
    if(!viewer.m_pageSizeOverride.has_value()) {
        viewer.m_pageSizeOverride = 500;
    }
    if(!viewer.m_iniPathOverride.has_value()) {
        viewer.m_iniPathOverride = m_tempDir->filePath("viewer.ini");
    }
    optsPriv.path = path;
    optsPriv.viewer_type = viewer.name();
    optsPriv.theme = 1;
    optsPriv.dpr = 1.0;
    opts.d_ptr = &optsPriv;
    viewer.load(nullptr, &opts);
}

void TestViewerPaging::initTestCase()
{
    m_tempDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tempDir->isValid());
    m_1mDbPath = m_tempDir->filePath("1m.db");
    m_smallDbPath = m_tempDir->filePath("small.db");
    m_longTextDbPath = m_tempDir->filePath("longtext.db");
    m_emptyPageDbPath = m_tempDir->filePath("emptypage.db");
    m_longTextValue = QString(5000, 'A');

    QVERIFY(createDatabase(m_1mDbPath, 1000000));
    QVERIFY(createDatabase(m_smallDbPath, 50));
    QVERIFY(createLongTextDatabase(m_longTextDbPath, 5000));
    QVERIFY(createDatabase(m_emptyPageDbPath, 1200));
}

void TestViewerPaging::cleanupTestCase()
{
    QFile::remove(m_1mDbPath);
    QFile::remove(m_smallDbPath);
    QFile::remove(m_longTextDbPath);
    QFile::remove(m_emptyPageDbPath);
    m_tempDir.reset();
}

void TestViewerPaging::test1MRowDatabaseOpensAndPages()
{
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, m_1mDbPath, optsPriv, opts);

    // Initial load displays table directly with ComboBox
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_tableCombo->isHidden(), 5000);
    QCOMPARE(viewer.m_tableCombo->currentText(), QString("items"));

    // The first page has to arrive before COUNT finishes; waiting for the
    // million-row total first would hide a regression that defers the page.
    QElapsedTimer timer;
    timer.start();
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->rowCount() == 500, 3000);
    QVERIFY(!viewer.m_pagerState.total.has_value());
    const qint64 firstPageElapsed = timer.elapsed();
    qInfo("First page loaded in %lld ms", firstPageElapsed);
    QVERIFY(firstPageElapsed < 3000);

    // Pager bar is visible and shows page 1
    QVERIFY(!viewer.m_pageBar->isHidden());
    QVERIFY(viewer.m_pageBar->shouldBeVisible());
    QCOMPARE(viewer.m_pagerState.page, 1LL);
    QCOMPARE(viewer.m_renderer->rowCount(), 500);

    // Wait for async COUNT to arrive
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_pagerState.total.has_value(), 5000);
    QCOMPARE(*viewer.m_pagerState.total, 1000000LL);
    QCOMPARE(viewer.m_pagerState.pages(), 2000LL);

    // Test next page navigation
    viewer.onNextPageClicked();
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 2, 2000);
    QCOMPARE(viewer.m_renderer->rowCount(), 500);

    // Test previous page navigation
    viewer.onPrevPageClicked();
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 1, 2000);
    QCOMPARE(viewer.m_renderer->rowCount(), 500);

    // Test last page navigation
    viewer.onLastPageClicked();
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 2000, 2000);
    QCOMPARE(viewer.m_pagerState.hasMore, false);
}

void TestViewerPaging::testSmallTableHidesPager()
{
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, m_smallDbPath, optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_tableCombo->isHidden(), 5000);
    QCOMPARE(viewer.m_tableCombo->currentText(), QString("items"));

    // Wait for count to land
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_pagerState.total.has_value(), 5000);
    QCOMPARE(*viewer.m_pagerState.total, 50LL);

    // Small table (50 <= 500) must hide the page bar
    QVERIFY(viewer.m_pageBar->isHidden());
    QVERIFY(!viewer.m_pageBar->shouldBeVisible());
}

void TestViewerPaging::testCsvPaged()
{
    // 1. Small CSV file: 1 page, pager bar hidden, sorting unsupported
    {
        DataTableViewer viewer;
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(viewer, QString(FIXTURES_DIR) + "/valid_basic.csv", optsPriv, opts);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);

        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->rowCount() > 0, 5000);

        // CSV/TSV is now paged
        QVERIFY(viewer.m_isPaged);
        QVERIFY(viewer.m_isCsv);
        QVERIFY(viewer.m_renderer->isPagedMode());

        // Small CSV hides the page bar
        QVERIFY(viewer.m_pageBar->isHidden());
        // The table selector belongs to SQLite files only
        QVERIFY(viewer.m_tableCombo->isHidden());
        // The filter box is disabled while loading; the first page must re-enable it
        QVERIFY(viewer.m_search->isEnabled());

        // Header click must not show sort indicator and must show unsupported explanation
        auto header = viewer.m_renderer->horizontalHeader();
        header->sectionClicked(0);
        QVERIFY(!header->isSortIndicatorShown());
        QCOMPARE(viewer.m_status->text(),
                 QString("Sorting is not supported for paged CSV/TSV files"));

        // Copying a cell works correctly
        viewer.m_renderer->selectCell(0, 0);
        viewer.m_renderer->copyToClipboard();
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 viewer.m_renderer->model()->index(0, 0).data().toString());
    }

    // 2. Large CSV file: > 500 rows, uses pager bar and navigates pages
    {
        QString largeCsvPath = m_tempDir->filePath("large_paged.csv");
        QFile file(largeCsvPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&file);
        out << "id,name,score\n";
        for(int i = 1; i <= 1250; ++i) {
            out << i << ",Item_" << i << "," << (i * 1.5) << "\n";
        }
        file.close();

        DataTableViewer viewer;
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(viewer, largeCsvPath, optsPriv, opts);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->rowCount() == 500, 5000);

        QVERIFY(viewer.m_isPaged);
        QVERIFY(viewer.m_isCsv);
        // Page bar must be visible for multi-page CSV
        QVERIFY(!viewer.m_pageBar->isHidden());
        QCOMPARE(viewer.m_pagerState.page, 1LL);

        // Wait for indexing to complete to verify total
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_pagerState.total.has_value(), 5000);
        QCOMPARE(*viewer.m_pagerState.total, 1250LL);
        QCOMPARE(viewer.m_pagerState.pages(), 3LL);

        // Verify status bar displays final totals upon completion
        QVERIFY(viewer.m_status->text().contains("1,250"));
        QVERIFY(!viewer.m_status->text().contains("Loading records:"));

        // Verify page-local filtering on CSV
        viewer.m_search->setText("Item_10");
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->filterMatchCount() < 500 &&
                                     viewer.m_renderer->filterMatchCount() > 0,
                                 2000);
        QVERIFY(viewer.m_status->text().contains("match on this page") ||
                viewer.m_status->text().contains("matches on this page"));
        viewer.m_search->clear();
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->filterMatchCount() == 500, 2000);

        // A clipped copy must survive the page turn that follows it
        viewer.m_renderer->selectCell(0, 1);
        viewer.m_renderer->copyToClipboard();
        viewer.onNextPageClicked();
        QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 2,
                                 2000);
        QCOMPARE(viewer.m_renderer->rowCount(), 500);
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("Item_1"));

        // Test Next page navigation to page 3 (partial page)
        viewer.onNextPageClicked();
        QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 3,
                                 2000);
        QCOMPARE(viewer.m_renderer->rowCount(), 250);
        QCOMPARE(viewer.m_pagerState.hasMore, false);

        // Test Previous page navigation back to page 2
        viewer.onPrevPageClicked();
        QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 2,
                                 2000);
        QCOMPARE(viewer.m_renderer->rowCount(), 500);

        // Test Last page navigation
        viewer.onLastPageClicked();
        QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 3,
                                 2000);
        QCOMPARE(viewer.m_renderer->rowCount(), 250);
        QCOMPARE(viewer.m_pagerState.hasMore, false);

        // Test First page navigation
        viewer.onFirstPageClicked();
        QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 1,
                                 2000);
        QCOMPARE(viewer.m_renderer->rowCount(), 500);

        // Test copy budget exceeded error feedback
        emit viewer.m_renderer->copyRefetchIncomplete(-1);
        QVERIFY(viewer.m_status->text().contains("Copy failed"));
        QVERIFY(viewer.m_status->text().contains("64 MiB"));

        // Verify cancelPending resets firstPagePending and resets state
        viewer.cancelPending();
        QVERIFY(!viewer.m_isPaged);
        QVERIFY(!viewer.m_firstPagePending);
    }

    // 3. Small TSV file: routing, format "TSV", pager bar hidden
    {
        DataTableViewer viewer;
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(viewer, QString(FIXTURES_DIR) + "/valid_diverse.tsv", optsPriv, opts);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->rowCount() > 0, 5000);

        QVERIFY(viewer.m_isPaged);
        QVERIFY(viewer.m_isCsv);
        QVERIFY(viewer.m_pageBar->isHidden());
        QVERIFY(viewer.m_tableCombo->isHidden());
        QVERIFY(viewer.m_status->text().contains("TSV"));

        // TSV header click must also explain that sorting is not supported
        auto header = viewer.m_renderer->horizontalHeader();
        header->sectionClicked(0);
        QVERIFY(!header->isSortIndicatorShown());
        QCOMPARE(viewer.m_status->text(),
                 QString("Sorting is not supported for paged CSV/TSV files"));
    }

    // 4. Clamped CSV cell: the copy must go through the async refetch and land
    //    the full value, resolved by the column labels in RefetchResult.
    {
        const QString longValue(6000, QChar('A'));
        QString clampedCsvPath = m_tempDir->filePath("clamped_paged.csv");
        QFile file(clampedCsvPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&file);
        out << "id,name,score\n";
        out << "1," << longValue << ",1.5\n";
        out << "2,Item_2,3\n";
        file.close();

        DataTableViewer viewer;
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(viewer, clampedCsvPath, optsPriv, opts);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->rowCount() == 2, 5000);

        // Page decode clamps the cell at the 4096-byte display cap, so the
        // stored text is shorter than the value on disk.
        QVERIFY(viewer.m_renderer->isCellClamped(0, 1));
        QVERIFY(viewer.m_renderer->model()->index(0, 1).data().toString().length() <
                longValue.length());

        viewer.m_renderer->selectCell(0, 1);
        viewer.m_renderer->copyToClipboard();
        QTRY_COMPARE_WITH_TIMEOUT(QGuiApplication::clipboard()->text(), longValue, 5000);

        viewer.m_renderer->copyAsMarkdown();
        QTRY_VERIFY_WITH_TIMEOUT(QGuiApplication::clipboard()->text().contains(longValue), 5000);
    }

    // 5. Asynchronously indexed 1-page CSV: 350 rows padded past the 256 KiB
    //    first scan chunk, so the sample scan cannot reach EOF and the first
    //    page has to wait for the background index. The page must arrive whole
    //    rather than as the partially indexed prefix, and the pager bar must
    //    stay hidden because 350 rows fit in one page.
    {
        QString async1PagePath = m_tempDir->filePath("async_1page.csv");
        QFile file(async1PagePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&file);
        out << "id,name,value\n";
        const QString filler(1000, QChar('x'));
        for(int i = 1; i <= 350; ++i) {
            out << i << ",Row_" << i << "," << filler << "\n";
        }
        file.close();
        QVERIFY(QFileInfo(async1PagePath).size() > 256 * 1024);

        DataTableViewer viewer;
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(viewer, async1PagePath, optsPriv, opts);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);
        // A synchronous source would have served the ~250 indexed records; only
        // a pending page that waited for the index can deliver all 350.
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->rowCount() == 350, 5000);

        // Wait for background indexing to complete
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_pagerState.total.has_value(), 5000);
        QCOMPARE(*viewer.m_pagerState.total, 350LL);
        QCOMPARE(viewer.m_pagerState.hasMore, false);

        // Pager bar must be hidden seamlessly for single page
        QVERIFY(viewer.m_pageBar->isHidden());
    }

    // 6. Back button state transition from SQLite table to CSV:
    //    Back button must be hidden immediately upon loading CSV.
    {
        DataTableViewer viewer;
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(viewer, m_smallDbPath, optsPriv, opts);
        QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_tableCombo->isHidden(), 5000);

        // Now load a CSV: table combo must be hidden immediately
        setupViewer(viewer, QString(FIXTURES_DIR) + "/valid_basic.csv", optsPriv, opts);
        QVERIFY(viewer.m_tableCombo->isHidden());
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);
        QVERIFY(viewer.m_tableCombo->isHidden());
    }

    // 7. Status bar indexing error preservation:
    {
        dtv::ui::StatusBar bar;
        bar.setPagedMode(true);
        bar.showLoading();
        bar.setIndexingProgress(100);
        bar.setIndexingFailed("Corrupt record at offset 1024");
        QVERIFY(bar.text().contains("Indexing error"));

        // When load info arrives after an indexing error, the error and warning
        // must be preserved rather than wiped out by summary text.
        bar.setPagedLoadInfo(1, 100, std::nullopt, 3, 4096, 15, "CSV",
                             "(built-in RFC 4180 parser)");
        QVERIFY(bar.text().contains("Indexing error"));
        QVERIFY(bar.text().contains("Corrupt record"));

        // When cell selection is cleared, restoreInfo must preserve the indexing error
        bar.setValueText("Col0 : 42");
        QCOMPARE(bar.text(), QString("Col0 : 42"));
        bar.restoreInfo();
        QVERIFY(bar.text().contains("Indexing error"));
        QVERIFY(bar.text().contains("Corrupt record"));
    }

    // 8. Multi-page CSV whose index cannot finish before the first page is
    //    served: the total is unknown at that moment and Next must stay
    //    available instead of presenting a temporary end as a final one.
    //    Rows are repeated from one block so the multi-megabyte fixture is
    //    written in a handful of large writes instead of line by line.
    {
        const QByteArray row = QByteArray("1,Row_1,") + QByteArray(280, 'x') + "\n";
        QByteArray block;
        for(int i = 0; i < 1000; ++i) {
            block += row;
        }
        const int kBlocks = 100;
        const int kRows = 1000 * kBlocks;

        QString bigCsvPath = m_tempDir->filePath("async_multipage.csv");
        QFile file(bigCsvPath);
        const QByteArray header = "id,name,value\n";
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(header) == header.size());
        for(int i = 0; i < kBlocks; ++i) {
            QVERIFY(file.write(block) == block.size());
        }
        file.close();

        DataTableViewer viewer;
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(viewer, bigCsvPath, optsPriv, opts);

        // QTRY_VERIFY polls in 50 ms steps, which is longer than the window in
        // which the page is served while indexing is still running, so poll
        // tightly to observe that window.
        for(int i = 0; i < 5000 && viewer.m_renderer->rowCount() != 500; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            QThread::msleep(1);
        }
        QCOMPARE(viewer.m_renderer->rowCount(), 500);
        QVERIFY(!viewer.m_pagerState.total.has_value());
        QVERIFY(viewer.m_pagerState.canNext());

        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_pagerState.total.has_value(), 30000);
        QCOMPARE(*viewer.m_pagerState.total, static_cast<int64_t>(kRows));
        // The progress line must not outlive completion.
        QVERIFY(!viewer.m_status->text().contains("Loading records"));
        QVERIFY(viewer.m_pagerState.canNext());

        viewer.onNextPageClicked();
        QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 2,
                                 10000);
        QCOMPARE(viewer.m_renderer->rowCount(), 500);
    }

    // 9. Index progress wiring. Real slice timing decides whether a throttled
    //    progress signal is emitted at all, so the progress, completion and
    //    failure signals are driven directly on a file that indexes in one go.
    //    Emitted from the UI thread the auto connection is direct, so the
    //    viewer's handler runs before the next statement.
    {
        QString progressPath = m_tempDir->filePath("progress.csv");
        QFile file(progressPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&file);
        out << "id,name,value\n";
        for(int i = 1; i <= 600; ++i) {
            out << i << ",Row_" << i << "," << i << "\n";
        }
        file.close();

        DataTableViewer viewer;
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(viewer, progressPath, optsPriv, opts);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->rowCount() == 500, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_pagerState.total.has_value(), 5000);
        QCOMPARE(*viewer.m_pagerState.total, 600LL);

        emit viewer.m_sourceWorker->indexProgress(viewer.m_generation, 12345, false, QString());
        QVERIFY(viewer.m_status->text().contains("Loading records"));
        QVERIFY(viewer.m_status->text().contains("12,345"));

        emit viewer.m_sourceWorker->indexProgress(viewer.m_generation, 600, true, QString());
        QCOMPARE(*viewer.m_pagerState.total, 600LL);
        QVERIFY(!viewer.m_status->text().contains("Loading records"));

        emit viewer.m_sourceWorker->indexProgress(viewer.m_generation, 600, false,
                                                  QString("Corrupt record at offset 1024"));
        QVERIFY(viewer.m_status->text().contains("Indexing error"));
        QVERIFY(viewer.m_status->text().contains("Corrupt record"));
    }

    // 10. Open failure handling: empty CSV file emits VCV_Error,
    //     cleans up pending operations, hides the loading indicator,
    //     and displays the error message.
    {
        QString emptyCsvPath = m_tempDir->filePath("empty.csv");
        QFile file(emptyCsvPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();

        DataTableViewer viewer;
        QSignalSpy spyCommand(&viewer, &ViewerBase::sigCommand);
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(viewer, emptyCsvPath, optsPriv, opts);

        QTRY_VERIFY_WITH_TIMEOUT(spyCommand.count() >= 2, 5000);
        QCOMPARE(spyCommand.last().at(0).toInt(), static_cast<int>(VCT_StateChange));
        QCOMPARE(spyCommand.last().at(1).toInt(), static_cast<int>(VCV_Error));

        QVERIFY(viewer.m_status->text().contains("Empty file"));
    }

    // 11. Multiline column header sanitization in Markdown export:
    //     A CSV with embedded newline in column header must not produce
    //     broken Markdown table headers across lines.
    {
        QString multilineHeaderPath = m_tempDir->filePath("multiline_header.csv");
        QFile file(multilineHeaderPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&file);
        out << "\"col\nline\",score\n";
        out << "A,1\n";
        file.close();

        DataTableViewer viewer;
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(viewer, multilineHeaderPath, optsPriv, opts);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->rowCount() == 1, 5000);

        viewer.m_renderer->selectCell(0, 0);
        viewer.m_renderer->copyAsMarkdown();
        QString md = QGuiApplication::clipboard()->text();
        QVERIFY(md.startsWith("|col line|\n|---|"));
        QVERIFY(!md.contains("col\nline"));
    }
}

void TestViewerPaging::testBackButtonTeardown()
{
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, m_smallDbPath, optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_tableCombo->isHidden(), 5000);

    // Verify Table ComboBox is visible and enabled even for single table (Requirement 2)
    QVERIFY(viewer.m_tableCombo != nullptr);
    QVERIFY(!viewer.m_tableCombo->isHidden());
    // The combo lives in the search bar, which the load disables until the
    // first page arrives. Enabled is the end state, not an instant one.
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_tableCombo->isEnabled(), 5000);
    QCOMPARE(viewer.m_tableCombo->currentText(), QString("items"));

    // Verify Tooltip contains the table note extracted from SQL DDL (Requirement 3)
    QVERIFY(viewer.m_tableCombo->toolTip().contains("Inventory items table"));

    // Verify ComboBox width adapts to item width and does not exceed the 36-char ceiling
    int textWidth = viewer.m_tableCombo->fontMetrics().horizontalAdvance("items");
    QVERIFY(viewer.m_tableCombo->width() >= textWidth);
    int max36Width = viewer.m_tableCombo->fontMetrics().horizontalAdvance(QString(36, '0')) + qRound(28 * 1.0);
    QVERIFY(viewer.m_tableCombo->width() <= max36Width);

    // Verify paged state is initialized
    QVERIFY(viewer.m_isPaged);
}

void TestViewerPaging::testHeaderStatePreservedAcrossPageTurns()
{
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, m_1mDbPath, optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_tableCombo->isHidden(), 5000);

    // Wait for first page
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 1, 2000);

    auto *header = viewer.m_renderer->horizontalHeader();
    QVERIFY(header != nullptr);

    // User modifies column 0 width to 260
    header->resizeSection(0, 260);
    QCOMPARE(header->sectionSize(0), 260);

    // Navigate to page 2
    viewer.onNextPageClicked();
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 2, 2000);

    // Column 0 width must still be 260, not wiped out by restoreHeaderState!
    QCOMPARE(header->sectionSize(0), 260);
}

void TestViewerPaging::testCountRequestedOnlyOnceOnPageTurns()
{
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, m_1mDbPath, optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_tableCombo->isHidden(), 5000);
    QTRY_VERIFY(viewer.m_countWorker != nullptr);

    // Count is requested once and completes
    QTRY_COMPARE_WITH_TIMEOUT(viewer.m_countCompletedCount, 1, 5000);
    QVERIFY(viewer.m_countRequested);

    // Navigate across pages
    viewer.onNextPageClicked();
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 2, 2000);
    QCOMPARE(viewer.m_countCompletedCount, 1);
    QVERIFY(viewer.m_countRequested);

    viewer.onNextPageClicked();
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 3, 2000);
    QCOMPARE(viewer.m_countCompletedCount, 1);
    QVERIFY(viewer.m_countRequested);
}

void TestViewerPaging::testPageNavigationFailureRollsBackPageNumber()
{
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, m_1mDbPath, optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_tableCombo->isHidden(), 5000);

    // Initial page 1 loaded
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 1, 2000);
    QCOMPARE(viewer.m_pagerState.page, 1LL);
    QVERIFY(viewer.m_pagerState.canNext());

    // Save valid token and invalidate current token to force worker query to fail
    auto savedToken = viewer.m_currentToken;
    viewer.m_currentToken.valid = false;

    // Attempt next page navigation
    viewer.onNextPageClicked();

    // Wait for query failure to be handled
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight, 2000);

    // Verify page number did not remain at 2; it rolled back to 1
    QCOMPARE(viewer.m_pagerState.page, 1LL);
    QVERIFY(viewer.m_status->text().contains("Error: Invalid page token"));

    // Restore valid token
    viewer.m_currentToken = savedToken;

    // Now next page succeeds cleanly and commits page 2
    viewer.onNextPageClicked();
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 2, 2000);
    QCOMPARE(viewer.m_pagerState.page, 2LL);
}

void TestViewerPaging::testLargeTableRowRangeDoesNotOverflow()
{
    dtv::core::PagerState state;
    state.pageSize = 500;
    state.page = 10000000LL;    // 10 millionth page
    state.total = 5000000000LL; // 5 billion rows (> 2.14B int32 limit)

    int64_t firstRow = dtv::core::firstRowOnPage(state.page, state.pageSize);
    int64_t lastRow = firstRow + 500 - 1;

    QVERIFY(firstRow > 2147483647LL);
    QVERIFY(lastRow > 2147483647LL);

    dtv::ui::StatusBar bar;
    bar.setPagedLoadInfo(firstRow, lastRow, state.total, 10, 1024, 50, "SQLite", "SQLite 3");

    QString text = bar.text();
    QVERIFY(text.contains(QString("%L1-%L2").arg(firstRow).arg(lastRow)));
    QVERIFY(!text.contains("---"));
}

void TestViewerPaging::testClampedLongTextCopyRefetchesFullContent()
{
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, m_longTextDbPath, optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_tableCombo->isHidden(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_renderer->rowCount() > 0, 5000);

    // The 5000-char name exceeds the 4096-byte page clamp, so the cell must be
    // flagged and its displayed value must be the clamped text.
    QVERIFY(viewer.m_renderer->isCellClamped(0, 1));
    QString clampedDisplay =
        viewer.m_renderer->model()->index(0, 1).data(Qt::DisplayRole).toString();
    QVERIFY(clampedDisplay.length() < m_longTextValue.length());

    // Plain copy must asynchronously refetch and produce the full unclamped value
    viewer.m_renderer->selectCell(0, 1);
    viewer.m_renderer->copyToClipboard();
    QTRY_COMPARE_WITH_TIMEOUT(QGuiApplication::clipboard()->text(), m_longTextValue, 5000);

    // Copy as Markdown must go through the same refetch and keep the full value.
    // Wait on the '|' prefix so the wait cannot be satisfied by the plain-text
    // result of the previous copy that is still on the clipboard.
    viewer.m_renderer->copyAsMarkdown();
    QTRY_VERIFY_WITH_TIMEOUT(QGuiApplication::clipboard()->text().startsWith("|"), 5000);
    QString mdText = QGuiApplication::clipboard()->text();
    QVERIFY(mdText.contains(m_longTextValue));
}

void TestViewerPaging::testEmptyPageNavigationPreservesStateAndToken()
{
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, m_emptyPageDbPath, optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_tableCombo->isHidden(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 1, 2000);

    // Go to page 2 (rows 501..1000)
    viewer.onNextPageClicked();
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 2, 2000);
    dtv::core::PageToken keptToken = viewer.m_currentToken;
    QVERIFY(keptToken.valid);

    // Externally delete rows 601..1200 so page 3 no longer has any rows
    sqlite3 *db = nullptr;
    QVERIFY(sqlite3_open(m_emptyPageDbPath.toUtf8().constData(), &db) == SQLITE_OK);
    QVERIFY(sqlite3_exec(db, "DELETE FROM items WHERE id > 600;", nullptr, nullptr, nullptr) ==
            SQLITE_OK);
    sqlite3_close(db);

    // Next hits the empty page: page number, rows and token must stay on page 2
    viewer.onNextPageClicked();
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_status->text().contains("No more rows exist"), 3000);
    QCOMPARE(viewer.m_pagerState.page, 2LL);
    QCOMPARE(viewer.m_currentToken.firstKey, keptToken.firstKey);
    QCOMPARE(viewer.m_currentToken.lastKey, keptToken.lastKey);
    QVERIFY(viewer.m_currentToken.valid);

    // Fresh COUNT rebuilds the pager bounds: total 600 -> 2 pages, no Next
    QTRY_VERIFY_WITH_TIMEOUT(
        viewer.m_pagerState.total.has_value() && *viewer.m_pagerState.total == 600LL, 3000);
    QVERIFY(!viewer.m_pagerState.canNext());
    QVERIFY(viewer.m_pagerState.canPrev());

    // Prev still works with the kept token
    viewer.onPrevPageClicked();
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 1, 2000);
}

void TestViewerPaging::testServerSortAndCancel()
{
    DataTableViewer viewer;
    ViewOptionsPrivate priv;
    ViewOptions opts;
    setupViewer(viewer, m_smallDbPath, priv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_tableCombo->isHidden() && !viewer.m_pageFetchInFlight &&
                             viewer.m_renderer->rowCount() > 0, 5000);
    viewer.m_pagerState.pageSize = 10;
    auto header = viewer.m_renderer->horizontalHeader();
    header->sectionClicked(0);
    QTRY_VERIFY(header->isSortIndicatorShown());
    QTRY_COMPARE(viewer.m_renderer->model()->index(0, 0).data().toString(), QString("1"));
    header->sectionClicked(0);
    QTRY_COMPARE(viewer.m_renderer->model()->index(0, 0).data().toString(), QString("50"));
    QCOMPARE(header->sortIndicatorOrder(), Qt::DescendingOrder);
    header->sectionClicked(1);
    QMetaObject::invokeMethod(&viewer, "cancelSort", Qt::DirectConnection);
    QTRY_VERIFY(!viewer.m_sorting);
    QCOMPARE(header->sortIndicatorSection(), 0);
    QCOMPARE(header->sortIndicatorOrder(), Qt::DescendingOrder);
    viewer.onNextPageClicked();
    QVERIFY(viewer.m_pageFetchInFlight);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 2, 2000);
    QCOMPARE(viewer.m_renderer->model()->index(0, 0).data().toString(), QString("40"));
    viewer.onFirstPageClicked();
    QVERIFY(viewer.m_pageFetchInFlight);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 1, 2000);
    QCOMPARE(viewer.m_renderer->model()->index(0, 0).data().toString(), QString("50"));
}

void TestViewerPaging::testRealHeaderClicksKeepCommittedIndicator()
{
    DataTableViewer viewer;
    ViewOptionsPrivate priv;
    ViewOptions opts;
    setupViewer(viewer, m_1mDbPath, priv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_tableCombo->isHidden() && !viewer.m_pageFetchInFlight &&
                             viewer.m_renderer->rowCount() > 0, 5000);
    viewer.resize(960, 600);
    viewer.show();
    QLabel *sortingLabel = nullptr;
    for(auto label : viewer.m_status->findChildren<QLabel *>()) {
        if(label->text().contains("href=\"cancel\""))
            sortingLabel = label;
    }
    QVERIFY(sortingLabel);
    auto header = viewer.m_renderer->horizontalHeader();
    auto click = [header](int column) {
        QTest::mouseClick(
            header->viewport(), Qt::LeftButton, Qt::NoModifier,
            QPoint(header->sectionViewportPosition(column) + 20, header->height() / 2), 0);
    };
    click(0);
    QVERIFY(viewer.m_sorting);
    QVERIFY(!sortingLabel->isHidden());
    QVERIFY(!header->isSortIndicatorShown());
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_sorting, 10000);
    QCOMPARE(header->sortIndicatorOrder(), Qt::AscendingOrder);

    viewer.onNextPageClicked();
    QVERIFY(viewer.m_pageFetchInFlight);
    click(1);
    QVERIFY(!viewer.m_sorting);
    QCOMPARE(header->sortIndicatorSection(), 0);
    QCOMPARE(header->sortIndicatorOrder(), Qt::AscendingOrder);
    QTRY_VERIFY(!viewer.m_pageFetchInFlight);

    click(0);
    QVERIFY(viewer.m_sorting);
    QCOMPARE(header->sortIndicatorOrder(), Qt::AscendingOrder);
    viewer.cancelSort();
    QVERIFY(viewer.m_recoveringSort);
    QVERIFY(sortingLabel->isHidden());
    click(1);
    QCOMPARE(header->sortIndicatorSection(), 0);
    QCOMPARE(header->sortIndicatorOrder(), Qt::AscendingOrder);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_sorting, 10000);

    click(0);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_sorting, 10000);
    QCOMPARE(header->sortIndicatorOrder(), Qt::DescendingOrder);
    QCOMPARE(viewer.m_renderer->model()->index(0, 0).data().toString(), QString("1000000"));
}

void TestViewerPaging::testLateSortCancelAndReplacement()
{
    DataTableViewer viewer;
    ViewOptionsPrivate priv;
    ViewOptions opts;
    setupViewer(viewer, m_1mDbPath, priv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_tableCombo->isHidden() && !viewer.m_pageFetchInFlight &&
                             viewer.m_renderer->rowCount() > 0, 5000);
    auto header = viewer.m_renderer->horizontalHeader();
    header->sectionClicked(0);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_sorting, 10000);
    header->sectionClicked(0);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_sorting, 10000);
    QCOMPARE(viewer.m_renderer->model()->index(0, 0).data().toString(), QString("1000000"));
    // Cancel from the worker finish boundary, after source order promotion.
    auto once = std::make_shared<std::atomic<bool>>(false);
    auto worker = viewer.m_sourceWorker;
    QMetaObject::invokeMethod(
        worker,
        [worker, &viewer, once] {
            worker->setFinishHook([&viewer, once] {
                if(!once->exchange(true)) {
                    QMetaObject::invokeMethod(
                        &viewer,
                        [&viewer] {
                            viewer.cancelSort();
                        },
                        Qt::BlockingQueuedConnection);
                }
            });
        },
        Qt::QueuedConnection);
    auto viewGen = viewer.m_viewGen->load();
    header->sectionClicked(1);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_sorting, 10000);
    QCOMPARE(viewer.m_viewGen->load(), viewGen);
    QCOMPARE(header->sortIndicatorSection(), 0);
    QCOMPARE(header->sortIndicatorOrder(), Qt::DescendingOrder);
    viewer.onNextPageClicked();
    QTRY_VERIFY(!viewer.m_pageFetchInFlight);
    QCOMPARE(viewer.m_renderer->model()->index(0, 0).data().toString(), QString("999500"));
    header->sectionClicked(1);
    header->sectionClicked(0);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_sorting, 10000);
    QCOMPARE(header->sortIndicatorSection(), 0);
    QCOMPARE(header->sortIndicatorOrder(), Qt::AscendingOrder);
    QCOMPARE(viewer.m_renderer->model()->index(0, 0).data().toString(), QString("1"));
}

void TestViewerPaging::testCopySurvivesPageTurnAndLatestWins()
{
    dtv::ui::TableRenderer renderer;
    auto data = std::make_shared<dtv::core::TableData>();
    data->columns.push_back({"text", dtv::core::ColumnMeta::Type::String});
    data->rows.push_back({"clamped"});
    dtv::core::RefetchKey key;
    key.rowid = 1;
    renderer.setPageData(data, {key}, {{true}});
    QSignalSpy requests(&renderer,
                        SIGNAL(refetchRowsRequested(
                            uint64_t, bool, std::vector<std::pair<int, dtv::core::RefetchKey>>)));
    renderer.selectCell(0, 0);
    renderer.copyToClipboard();
    QVERIFY(requests.isValid());
    QCOMPARE(requests.count(), 1);
    auto request = requests.last().at(0).toULongLong();
    renderer.setPageData(data, {key}, {{false}});
    dtv::core::RefetchResult result;
    result.ok = true;
    result.values = {QString(2000, QChar(0x4e2d)).toStdString() + "\r\nend"};
    renderer.onRefetchRowsCompleted(request, {{0, result}});
    QTRY_COMPARE_WITH_TIMEOUT(QGuiApplication::clipboard()->text(),
                              QString(2000, QChar(0x4e2d)) + " end", 3000);
    QTest::qWait(50);
    renderer.setPageData(data, {key}, {{true}});
    renderer.selectCell(0, 0);
    renderer.copyToClipboard();
    auto oldRequest = requests.last().at(0).toULongLong();
    renderer.setPageData(data, {key}, {{false}});
    renderer.selectCell(0, 0);
    renderer.copyToClipboard();
    renderer.onRefetchRowsCompleted(oldRequest, {{0, result}});
    QTRY_COMPARE_WITH_TIMEOUT(QGuiApplication::clipboard()->text(), QString("clamped"), 3000);
    QTest::qWait(50);
    renderer.setPageData(data, {key}, {{true}});
    renderer.selectCell(0, 0);
    renderer.copyAsMarkdown();
    request = requests.last().at(0).toULongLong();
    auto next = std::make_shared<dtv::core::TableData>(*data);
    next->columns[0].name = "changed";
    renderer.setPageData(next, {key}, {{false}});
    renderer.onRefetchRowsCompleted(request, {{0, result}});
    QTRY_VERIFY_WITH_TIMEOUT(QGuiApplication::clipboard()->text().startsWith("|text|"), 3000);
    QVERIFY(QGuiApplication::clipboard()->text().contains(QString(2000, QChar(0x4e2d)) + " end"));
}

void TestViewerPaging::testTextViewControlBarButton()
{
    // 1. With control bar and valid path: button created, enabled, emits VCT_LoadViewerWithNewType "Text"
    {
        DataTableViewer viewer;
        QHBoxLayout ctrlbarLayout;

        ViewOptionsPrivate optsPriv;
        optsPriv.path = m_smallDbPath;
        optsPriv.viewer_type = viewer.name();
        optsPriv.theme = 1;
        optsPriv.dpr = 1.0;
        ViewOptions opts;
        opts.d_ptr = &optsPriv;

        viewer.m_iniPathOverride = m_tempDir->filePath("viewer.ini");
        viewer.load(&ctrlbarLayout, &opts);

        QVERIFY(viewer.m_btnTextView != nullptr);
        QVERIFY(viewer.m_btnTextView->isEnabled());
        QCOMPARE(viewer.m_btnTextView->toolTip(), QString("View in Text viewer (Ctrl+Alt+T)"));

        // Verify action is registered in ActionRegistry with default Ctrl+Alt+T
        QVERIFY(viewer.actionRegistry() != nullptr);
        QAction *viewTextAction = viewer.actionRegistry()->action("DataTableViewer.viewText");
        QVERIFY(viewTextAction != nullptr);
        QCOMPARE(viewTextAction->shortcut(), QKeySequence("Ctrl+Alt+T"));
        QVERIFY(viewTextAction->isEnabled());

        QSignalSpy spyCommand(&viewer, &ViewerBase::sigCommand);

        viewer.m_btnTextView->click();

        QCOMPARE(spyCommand.count(), 1);
        QCOMPARE(spyCommand.at(0).at(0).toInt(), static_cast<int>(VCT_LoadViewerWithNewType));
        QCOMPARE(spyCommand.at(0).at(1).toString(), QString("Text"));

        // Triggering action directly also emits the command
        viewTextAction->trigger();
        QCOMPARE(spyCommand.count(), 2);
        QCOMPARE(spyCommand.at(1).at(0).toInt(), static_cast<int>(VCT_LoadViewerWithNewType));
        QCOMPARE(spyCommand.at(1).at(1).toString(), QString("Text"));

        // DPR and theme updates scale the button size properly
        viewer.updateDPR(1.5);
        QCOMPARE(viewer.m_btnTextView->width(), qRound(30 * 1.5));
        QCOMPARE(viewer.m_btnTextView->iconSize(), QSize(qRound(24 * 1.5), qRound(24 * 1.5)));
        QVERIFY(!viewer.m_btnTextView->icon().availableSizes().isEmpty());
        QCOMPARE(viewer.m_btnTextView->icon().availableSizes().first(),
                 QSize(qRound(24 * 1.5), qRound(24 * 1.5)));
        viewer.updateTheme(1); // Dark theme
    }

    // 2. Disabled state with empty path
    {
        DataTableViewer viewer;
        QHBoxLayout ctrlbarLayout;

        ViewOptionsPrivate optsPriv;
        optsPriv.path = "";
        optsPriv.viewer_type = viewer.name();
        optsPriv.theme = 1;
        optsPriv.dpr = 1.0;
        ViewOptions opts;
        opts.d_ptr = &optsPriv;

        viewer.m_iniPathOverride = m_tempDir->filePath("viewer.ini");
        viewer.load(&ctrlbarLayout, &opts);

        QVERIFY(viewer.m_btnTextView != nullptr);
        QVERIFY(!viewer.m_btnTextView->isEnabled());

        QAction *viewTextAction = viewer.actionRegistry()->action("DataTableViewer.viewText");
        QVERIFY(viewTextAction != nullptr);
        QVERIFY(!viewTextAction->isEnabled());

        QSignalSpy spyCommand(&viewer, &ViewerBase::sigCommand);
        viewer.m_btnTextView->click();
        QCOMPARE(spyCommand.count(), 0);

        viewTextAction->trigger();
        QCOMPARE(spyCommand.count(), 0);

        viewer.onTextViewBtnClicked();
        QCOMPARE(spyCommand.count(), 0);

        // Verify disabled icon has dimmed opacity (alpha <= 105)
        QImage normalImg =
            viewer.m_btnTextView->icon().pixmap(QSize(24, 24), QIcon::Normal).toImage();
        bool foundOpaqueInNormal = false;
        for(int y = 0; y < normalImg.height(); ++y) {
            for(int x = 0; x < normalImg.width(); ++x) {
                if(qAlpha(normalImg.pixel(x, y)) > 200) {
                    foundOpaqueInNormal = true;
                    break;
                }
            }
        }
        QVERIFY(foundOpaqueInNormal);

        QImage disabledImg =
            viewer.m_btnTextView->icon().pixmap(QSize(24, 24), QIcon::Disabled).toImage();
        bool foundOver105InDisabled = false;
        for(int y = 0; y < disabledImg.height(); ++y) {
            for(int x = 0; x < disabledImg.width(); ++x) {
                if(qAlpha(disabledImg.pixel(x, y)) > 105) {
                    foundOver105InDisabled = true;
                    break;
                }
            }
        }
        QVERIFY(!foundOver105InDisabled);
    }

    // 3. Null control bar: the button still exists and stays usable, the action
    //    is reachable and emits, and nothing crashes when the layout is absent.
    {
        DataTableViewer viewer;

        ViewOptionsPrivate optsPriv;
        optsPriv.path = m_smallDbPath;
        optsPriv.viewer_type = viewer.name();
        optsPriv.theme = 1;
        optsPriv.dpr = 1.0;
        ViewOptions opts;
        opts.d_ptr = &optsPriv;

        viewer.m_iniPathOverride = m_tempDir->filePath("viewer.ini");
        viewer.load(nullptr, &opts);
        // Without show() the button is hidden by construction, so the
        // assertion below would pass even if the code stopped hiding it.
        viewer.show();

        // Created unconditionally, merely not attached to a layout.
        QVERIFY(viewer.m_btnTextView != nullptr);
        QCOMPARE(viewer.m_btnTextView->parentWidget(), static_cast<QWidget *>(&viewer));
        QVERIFY(viewer.m_btnTextView->isEnabled());
        // It is in no layout, so it must stay hidden: otherwise the host
        // showing the viewer would paint a 30x30 button at (0,0) over the
        // search bar.
        QVERIFY(!viewer.m_btnTextView->isVisible());

        QSignalSpy spyCommand(&viewer, &ViewerBase::sigCommand);
        viewer.onTextViewBtnClicked();
        QCOMPARE(spyCommand.count(), 1);
        QCOMPARE(spyCommand.at(0).at(1).toString(), QString("Text"));

        viewer.updateDPR(2.0);
        viewer.updateTheme(0);
        QCOMPARE(viewer.m_btnTextView->width(), qRound(30 * 2.0));
    }

    // 3b. Null control bar with an empty path: action stays disabled and silent.
    {
        DataTableViewer viewer;

        ViewOptionsPrivate optsPriv;
        optsPriv.path = "";
        optsPriv.viewer_type = viewer.name();
        optsPriv.theme = 1;
        optsPriv.dpr = 1.0;
        ViewOptions opts;
        opts.d_ptr = &optsPriv;

        viewer.m_iniPathOverride = m_tempDir->filePath("viewer.ini");
        viewer.load(nullptr, &opts);

        QVERIFY(viewer.m_btnTextView != nullptr);
        QVERIFY(!viewer.m_btnTextView->isEnabled());

        QAction *viewTextAction = viewer.actionRegistry()->action("DataTableViewer.viewText");
        QVERIFY(viewTextAction != nullptr);
        QVERIFY(!viewTextAction->isEnabled());

        QSignalSpy spyCommand(&viewer, &ViewerBase::sigCommand);
        viewTextAction->trigger();
        QCOMPARE(spyCommand.count(), 0);
        viewer.onTextViewBtnClicked();
        QCOMPARE(spyCommand.count(), 0);
    }

    // 4. Pre-init updateDPR/updateTheme does not crash
    {
        DataTableViewer viewer;
        viewer.updateDPR(1.5);
        viewer.updateTheme(0);
        viewer.updateTheme(1);
    }

    // 5. External deletion of m_btnTextView zeroes the QPointer and the later
    //    refreshes stay safe. Nothing in the plugin deletes the button; this
    //    probes the guard robustness, not a teardown path the plugin performs.
    {
        DataTableViewer viewer;
        QHBoxLayout ctrlbarLayout;

        ViewOptionsPrivate optsPriv;
        optsPriv.path = m_smallDbPath;
        optsPriv.viewer_type = viewer.name();
        optsPriv.theme = 1;
        optsPriv.dpr = 1.0;
        ViewOptions opts;
        opts.d_ptr = &optsPriv;

        viewer.m_iniPathOverride = m_tempDir->filePath("viewer.ini");
        viewer.load(&ctrlbarLayout, &opts);
        QVERIFY(viewer.m_btnTextView != nullptr);

        // The deleted button must not be touched again when the theme or DPR
        // changes: that is the behaviour under test, not the QPointer reset.
        delete viewer.m_btnTextView.data();

        viewer.updateDPR(1.5);
        viewer.updateTheme(0);
    }

    // 6. Custom shortcut override from settings is reflected in action and tooltip
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString iniFile = tempDir.filePath("DataTableViewer.ini");
        {
            QSettings customIni(iniFile, QSettings::IniFormat);
            customIni.beginGroup("Shortcuts");
            customIni.setValue("DataTableViewer.viewText", "Ctrl+Shift+T");
            customIni.endGroup();
            customIni.sync();
        }

        DataTableViewer viewer;
        viewer.m_iniPathOverride = iniFile;
        QHBoxLayout ctrlbarLayout;

        ViewOptionsPrivate optsPriv;
        optsPriv.path = m_smallDbPath;
        optsPriv.viewer_type = viewer.name();
        optsPriv.theme = 0;
        optsPriv.dpr = 1.0;
        ViewOptions opts;
        opts.d_ptr = &optsPriv;

        viewer.load(&ctrlbarLayout, &opts);

        QVERIFY(viewer.m_btnTextView != nullptr);
        QCOMPARE(viewer.m_btnTextView->toolTip(), QString("View in Text viewer (Ctrl+Shift+T)"));
        QAction *viewTextAction = viewer.actionRegistry()->action("DataTableViewer.viewText");
        QVERIFY(viewTextAction != nullptr);
        QCOMPARE(viewTextAction->shortcut(), QKeySequence("Ctrl+Shift+T"));
        QVERIFY(viewTextAction->isEnabled());
    }
}

void TestViewerPaging::testRowIndexColumn()
{
    // 1. Default row_index_b = true: vertical header is visible and reports 1-based data row numbers
    {
        DataTableViewer viewer;
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(viewer, m_smallDbPath, optsPriv, opts);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->rowCount() > 0, 5000);

        QVERIFY(viewer.m_renderer->showRowIndex());
        auto *table = viewer.m_renderer->findChild<QTableView *>();
        QVERIFY(table != nullptr);
        auto *vHeader = table->verticalHeader();
        QVERIFY(vHeader != nullptr);
        QVERIFY(!vHeader->isHidden());
        QVERIFY(vHeader->width() > 0);

        auto *model = viewer.m_renderer->model();
        QVERIFY(model != nullptr);
        QCOMPARE(model->headerData(0, Qt::Vertical, Qt::DisplayRole).toString(), QString("1"));
        QCOMPARE(model->headerData(1, Qt::Vertical, Qt::DisplayRole).toString(), QString("2"));
        QCOMPARE(model->headerData(2, Qt::Vertical, Qt::DisplayRole).toString(), QString("3"));

        // Copy 2x2 selection: payload must contain only cell data, no row numbers
        auto *proxyModel = table->model();
        QItemSelection sel(proxyModel->index(0, 0), proxyModel->index(1, 1));
        table->selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
        viewer.m_renderer->copyToClipboard();
        QString clipboardText = QGuiApplication::clipboard()->text();
        QVERIFY(!clipboardText.isEmpty());
        // Must contain 2 columns per row ("id\tname"), isolating the vertical header
        QStringList lines = clipboardText.split('\n', Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines[0], QString("1\tItem_1"));
        QCOMPARE(lines[1], QString("2\tItem_2"));
    }

    // 2. Setting row_index_b = false in INI: vertical header stays hidden
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString customIni = tempDir.filePath("no_row_index.ini");
        {
            QSettings settings(customIni, QSettings::IniFormat);
            settings.setValue("row_index_b", false);
            settings.sync();
        }

        DataTableViewer viewer;
        viewer.m_iniPathOverride = customIni;
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(viewer, m_smallDbPath, optsPriv, opts);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->rowCount() > 0, 5000);

        QVERIFY(!viewer.m_renderer->showRowIndex());
        auto *table = viewer.m_renderer->findChild<QTableView *>();
        QVERIFY(table != nullptr);
        auto *vHeader = table->verticalHeader();
        QVERIFY(vHeader != nullptr);
        QVERIFY(vHeader->isHidden());
    }

    // 3. Filtering: data-row numbering remains honest (non-contiguous)
    {
        DataTableViewer viewer;
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(viewer, m_smallDbPath, optsPriv, opts);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->rowCount() > 0, 5000);

        // Filter for "Item_2" (matches Item_2 and Item_20..Item_29 -> 11 rows)
        viewer.m_renderer->setFilter("Item_2", -1);
        QTRY_COMPARE_WITH_TIMEOUT(viewer.m_renderer->filterMatchCount(), 11, 2000);

        auto *table = viewer.m_renderer->findChild<QTableView *>();
        auto *vHeader = table->verticalHeader();
        QVERIFY(!vHeader->isHidden());
        // Source row indices produce non-contiguous vertical header numbering:
        // Item_2 is source row 1 -> "2" (displayed at visual row 0);
        // Item_20 is source row 19 -> "20" (displayed at visual row 1)
        auto *proxyModel = table->model();
        QCOMPARE(proxyModel->headerData(0, Qt::Vertical, Qt::DisplayRole).toString(), QString("2"));
        QCOMPARE(proxyModel->headerData(1, Qt::Vertical, Qt::DisplayRole).toString(),
                 QString("20"));
    }

    // 4. Paging: vertical header displays honest global row numbers across pages (e.g. 501..1000 on page 2)
    {
        // Not m_emptyPageDbPath: that fixture is truncated to 600 rows by
        // testEmptyPageNavigationPreservesStateAndToken, and QtTest runs slots in
        // declaration order, so it can no longer deliver a second full 500-row page.
        DataTableViewer viewer;
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(viewer, m_1mDbPath, optsPriv, opts);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->rowCount() == 500, 5000);

        auto *table = viewer.m_renderer->findChild<QTableView *>();
        QVERIFY(table != nullptr);
        auto *proxyModel = table->model();
        QVERIFY(proxyModel != nullptr);

        // Page 1: rows 1..500
        QCOMPARE(proxyModel->headerData(0, Qt::Vertical, Qt::DisplayRole).toString(), QString("1"));
        QCOMPARE(proxyModel->headerData(499, Qt::Vertical, Qt::DisplayRole).toString(),
                 QString("500"));

        // Navigate to Page 2
        viewer.onNextPageClicked();
        QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 2,
                                 5000);
        QCOMPARE(viewer.m_renderer->rowCount(), 500);

        // Page 2: rows 501..1000
        QCOMPARE(proxyModel->headerData(0, Qt::Vertical, Qt::DisplayRole).toString(),
                 QString("501"));
        QCOMPARE(proxyModel->headerData(499, Qt::Vertical, Qt::DisplayRole).toString(),
                 QString("1000"));
    }
}

void TestViewerPaging::testStatusBarRowIndexMetrics()
{
    // 1. Single-page table (no paging): displays [Row n/total] without Global #
    {
        DataTableViewer viewer;
        QSignalSpy spyProps(&viewer, &ViewerBase::sigCommand);
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(viewer, m_smallDbPath, optsPriv, opts);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->rowCount() > 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_pagerState.total.has_value(), 5000);

        // Select cell (0, 0) -> row 1 of 50 (no pagination, Global # omitted)
        viewer.m_renderer->selectCell(0, 0);
        QCOMPARE(viewer.m_status->text(), QString("[Row 1/50] id : 1"));

        // Select cell (4, 1) -> row 5 of 50
        viewer.m_renderer->selectCell(4, 1);
        QCOMPARE(viewer.m_status->text(), QString("[Row 5/50] name : Item_5"));

        // Filter for "Item_2" via search bar (matches 11 rows: Item_2, Item_20..Item_29)
        viewer.m_search->setText("Item_2");
        QTRY_COMPARE_WITH_TIMEOUT(viewer.m_renderer->filterMatchCount(), 11, 2000);

        // Select first visible row (Item_2, source row index 1)
        viewer.m_renderer->selectCell(1, 1);
        QCOMPARE(viewer.m_status->text(),
                 QString("[11 matches on this page]  [Row 2/50] name : Item_2"));

        // Verify inspector property was emitted to host
        bool foundPropCmd = false;
        for(int i = 0; i < spyProps.count(); ++i) {
            if(spyProps.at(i).at(0).toInt() == static_cast<int>(VCT_AppendProperty)) {
                foundPropCmd = true;
                auto props = spyProps.at(i).at(1).value<QVector<QPair<QString, QString>>>();
                QVERIFY(!props.isEmpty());
                break;
            }
        }
        QVERIFY(foundPropCmd);

        // Clear filter and clear selection
        viewer.m_search->clear();
        auto *table = viewer.m_renderer->findChild<QTableView *>();
        table->selectionModel()->clearSelection();
        table->selectionModel()->setCurrentIndex(QModelIndex(), QItemSelectionModel::NoUpdate);
        QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_status->text().contains("[Row"), 2000);
    }

    // 2. Multi-page table (paging active): displays [Row n/pageSize, Global #k/total]
    {
        DataTableViewer viewer;
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(viewer, m_1mDbPath, optsPriv, opts);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->rowCount() > 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 1, 5000);

        // Select cell (0, 0) -> row 1 of 500, global #1
        viewer.m_renderer->selectCell(0, 0);
        QVERIFY(viewer.m_status->text().startsWith("[Row 1/500, Global #1"));
        QVERIFY(viewer.m_status->text().contains("id : 1"));

        // Select cell (4, 1) -> row 5 of 500, global #5
        viewer.m_renderer->selectCell(4, 1);
        QVERIFY(viewer.m_status->text().startsWith("[Row 5/500, Global #5"));
        QVERIFY(viewer.m_status->text().contains("name : Item_5"));
    }

    // 3. Single-page CSV table: displays [Row n/total] without Global #
    {
        DataTableViewer viewer;
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(viewer, QString(FIXTURES_DIR) + "/valid_basic.csv", optsPriv, opts);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->rowCount() > 0, 5000);

        // Select cell (1, 0) -> row 2 of 3
        viewer.m_renderer->selectCell(1, 0);
        QCOMPARE(viewer.m_status->text(), QString("[Row 2/3] id : 2"));
    }
}

void TestViewerPaging::testPagingShortcutsAndTooltips()
{
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, m_1mDbPath, optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_renderer->rowCount() == 500, 5000);

    // 1. Verify PageBar button tooltips display native shortcut hints
    const QList<QPushButton *> buttons = viewer.m_pageBar->findChildren<QPushButton *>();
    QCOMPARE(buttons.size(), 4);

    const QString hintFirst =
        QKeySequence(Qt::ControlModifier | Qt::Key_Home).toString(QKeySequence::NativeText);
    const QString hintPrev =
        QKeySequence(Qt::ControlModifier | Qt::Key_PageUp).toString(QKeySequence::NativeText);
    const QString hintNext =
        QKeySequence(Qt::ControlModifier | Qt::Key_PageDown).toString(QKeySequence::NativeText);
    const QString hintLast =
        QKeySequence(Qt::ControlModifier | Qt::Key_End).toString(QKeySequence::NativeText);

    QCOMPARE(buttons.at(0)->toolTip(), QString("First page (%1)").arg(hintFirst));
    QCOMPARE(buttons.at(1)->toolTip(), QString("Previous page (%1)").arg(hintPrev));
    QCOMPARE(buttons.at(2)->toolTip(), QString("Next page (%1)").arg(hintNext));
    QCOMPARE(buttons.at(3)->toolTip(), QString("Last page (%1)").arg(hintLast));

    // 2. Verify registered actions in ActionRegistry
    QVERIFY(viewer.actionRegistry() != nullptr);
    QAction *actFirst = viewer.actionRegistry()->action("DataTableViewer.pageFirst");
    QAction *actPrev = viewer.actionRegistry()->action("DataTableViewer.pagePrev");
    QAction *actNext = viewer.actionRegistry()->action("DataTableViewer.pageNext");
    QAction *actLast = viewer.actionRegistry()->action("DataTableViewer.pageLast");
    QVERIFY(actFirst && actPrev && actNext && actLast);
    QCOMPARE(actFirst->shortcut(), QKeySequence(Qt::ControlModifier | Qt::Key_Home));
    QCOMPARE(actPrev->shortcut(), QKeySequence(Qt::ControlModifier | Qt::Key_PageUp));
    QCOMPARE(actNext->shortcut(), QKeySequence(Qt::ControlModifier | Qt::Key_PageDown));
    QCOMPARE(actLast->shortcut(), QKeySequence(Qt::ControlModifier | Qt::Key_End));

    // 3. Triggering pageNext action navigates to page 2
    QCOMPARE(viewer.m_pagerState.page, 1LL);
    actNext->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 2, 5000);
    QCOMPARE(viewer.m_renderer->rowCount(), 500);

    // 4. Triggering pagePrev action navigates back to page 1
    actPrev->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 1, 5000);
    QCOMPARE(viewer.m_renderer->rowCount(), 500);

    // 5. Verify eventFilter ignores modified PageUp/Down at boundaries
    auto *table = viewer.m_renderer->findChild<QTableView *>();
    QVERIFY(table != nullptr);
    QKeyEvent shiftPageUp(QEvent::KeyPress, Qt::Key_PageUp, Qt::ShiftModifier);
    QCoreApplication::sendEvent(table, &shiftPageUp);
    // Must not turn page: remains on page 1
    QCOMPARE(viewer.m_pagerState.page, 1LL);
}

void TestViewerPaging::testCancelPendingClearsForeignPagerState() {
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;

    // A million-row table first: its total is the one that must not survive.
    setupViewer(viewer, m_1mDbPath, optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_pagerState.total.has_value(), 5000);
    QCOMPARE(*viewer.m_pagerState.total, 1000000LL);
    QVERIFY(viewer.m_currentToken.valid);
    QVERIFY(viewer.m_colCount > 0);

    viewer.cancelPending();

    QVERIFY(!viewer.m_pagerState.total.has_value());
    QVERIFY(!viewer.m_currentToken.valid);
    QCOMPARE(viewer.m_colCount, 0);
    QCOMPARE(viewer.m_pagerState.page, 1LL);
    QVERIFY(!viewer.m_pagerState.hasMore);

    // Observable consequence: switching to a 50-row table has to end up with
    // that table's own count. A surviving total would both display the wrong
    // figure and suppress this load's COUNT request.
    setupViewer(viewer, m_smallDbPath, optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 1, 5000);
    QVERIFY(!viewer.m_pagerState.total.has_value() || *viewer.m_pagerState.total == 50LL);
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_pagerState.total.has_value(), 5000);
    QCOMPARE(*viewer.m_pagerState.total, 50LL);
}

void TestViewerPaging::testContentSizingAndPropertyBounds()
{
    // 1. Verify getContentSize() returns natural size 960x600 scaled by DPR
    {
        DataTableViewer viewer;
        QCOMPARE(viewer.getContentSize(), QSize(960, 600));

        viewer.updateDPR(1.5);
        QCOMPARE(viewer.getContentSize(), QSize(qRound(960 * 1.5), qRound(600 * 1.5)));

        viewer.updateDPR(2.0);
        QCOMPARE(viewer.getContentSize(), QSize(960 * 2, 600 * 2));
    }

    // 2. Setting size_min_viewer and size_max_viewer in properties applies to viewer widget bounds
    {
        DataTableViewer viewer;
        ViewOptionsPrivate optsPriv;
        optsPriv.path = m_smallDbPath;
        optsPriv.viewer_type = viewer.name();
        optsPriv.extras.insert(ViewOptionsKeys::kKeySizeMin, QSize(400, 300));
        optsPriv.extras.insert(ViewOptionsKeys::kKeySizeMax, QSize(1600, 1200));

        ViewOptions opts;
        opts.d_ptr = &optsPriv;

        QHBoxLayout ctrlbarLayout;
        viewer.m_iniPathOverride = m_tempDir->filePath("viewer.ini");
        viewer.load(&ctrlbarLayout, &opts);

        QCOMPARE(viewer.minimumSize(), QSize(400, 300));
        QCOMPARE(viewer.maximumSize(), QSize(1600, 1200));
    }

    // 3. In the absence of sizing properties, default widget min constraints are preserved
    {
        DataTableViewer viewer;
        ViewOptionsPrivate optsPriv;
        optsPriv.path = m_smallDbPath;
        optsPriv.viewer_type = viewer.name();

        ViewOptions opts;
        opts.d_ptr = &optsPriv;

        QHBoxLayout ctrlbarLayout;
        viewer.m_iniPathOverride = m_tempDir->filePath("viewer.ini");
        viewer.load(&ctrlbarLayout, &opts);

        QCOMPARE(viewer.minimumSize(), QSize(0, 0));
    }
}

void TestViewerPaging::testRepeatedIndexingErrorDoesNotAccumulate()
{
    // setPagedLoadInfo() re-applies a stored indexing error on every page turn,
    // and setIndexingFailed() can be re-reported by the worker. Neither may
    // append a second copy of the same warning to the summary or the tooltip.
    dtv::ui::StatusBar bar;
    bar.setPagedMode(true);
    bar.showLoading();
    bar.setPagedLoadInfo(1, 500, std::nullopt, 3, 4096, 15, "CSV", "(built-in RFC 4180 parser)");
    bar.setIndexingFailed("Corrupt record at offset 1024");

    const QString afterFirst = bar.text();
    const int firstCount = afterFirst.count("Warning:");

    // A page turn re-applies the same stored error through setPagedLoadInfo.
    bar.setPagedLoadInfo(501, 1000, std::nullopt, 3, 4096, 18, "CSV", "(built-in RFC 4180 parser)");
    QCOMPARE(bar.text().count("Warning:"), firstCount);
    QCOMPARE(bar.text().count("Corrupt record"), 1);

    // A direct re-report of the same error must also be idempotent.
    bar.setIndexingFailed("Corrupt record at offset 1024");
    QCOMPARE(bar.text().count("Warning:"), firstCount);
    QCOMPARE(bar.text().count("Corrupt record"), 1);

    // A different error still lands, and still only once.
    bar.setIndexingFailed("Corrupt record at offset 2048");
    QCOMPARE(bar.text().count("Corrupt record at offset 2048"), 1);

    // showLoading() drops the warning entirely rather than carrying it forward.
    bar.showLoading();
    QVERIFY(!bar.text().contains("Warning:"));
}

void TestViewerPaging::testViewerDestructionJoinsActiveWorkers()
{
    // Test that destroying a viewer while background workers are actively
    // running or starting joins the threads cleanly without crashing or double-free.
    {
        auto viewer = std::make_unique<DataTableViewer>();
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(*viewer, m_1mDbPath, optsPriv, opts);
        viewer->loadSelectedTable(m_1mDbPath, "items");
        // Destroy the viewer immediately while workers/threads are in-flight
        viewer.reset();
    }
    // Also test with a viewer that is navigated mid-flight
    {
        auto viewer = std::make_unique<DataTableViewer>();
        ViewOptionsPrivate optsPriv;
        ViewOptions opts;
        setupViewer(*viewer, m_1mDbPath, optsPriv, opts);
        QTRY_VERIFY_WITH_TIMEOUT(viewer->m_stack->currentWidget() == viewer->m_renderer, 5000);
        // Trigger a page navigation
        viewer->onNextPageClicked();
        // Destroy while page query is in-flight
        viewer.reset();
    }
}

void TestViewerPaging::testCopyBudgetDirectPathAccuracy()
{
    dtv::ui::TableRenderer renderer;
    auto data = std::make_shared<dtv::core::TableData>();
    data->columns.push_back({"col1", dtv::core::ColumnMeta::Type::String});

    // Case 1: An ASCII cell that is 25 MB in size.
    // Under the old heuristic (size * 3 + 1), 25 MB was estimated as 75 MB and rejected (> 64 MB).
    // Under the accurate utf8ByteCount metric, 25 MB is 25 MB (< 64 MB), so direct copy succeeds!
    std::string text25MB(25 * 1024 * 1024, 'x');
    data->rows.push_back({text25MB});
    dtv::core::RefetchKey key;
    key.rowid = 1;
    renderer.setPageData(data, {key}, {{false}});

    QSignalSpy incompleteSpy(&renderer, SIGNAL(copyRefetchIncomplete(int)));
    renderer.selectCell(0, 0);
    renderer.copyToClipboard();

    // Must NOT be refused
    QCOMPARE(incompleteSpy.count(), 0);
    QCOMPARE(QGuiApplication::clipboard()->text().size(), static_cast<int>(text25MB.size()));

    // Case 2: An oversized cell exceeding 64 MiB (e.g., 65 MiB)
    std::string text65MB(65 * 1024 * 1024, 'y');
    data->rows[0] = {text65MB};
    renderer.setPageData(data, {key}, {{false}});

    QGuiApplication::clipboard()->clear();
    renderer.selectCell(0, 0);
    renderer.copyToClipboard();

    // Must be rejected with incomplete (-1) and clipboard remains empty
    QCOMPARE(incompleteSpy.count(), 1);
    QCOMPARE(incompleteSpy.last().at(0).toInt(), -1);
    QVERIFY(QGuiApplication::clipboard()->text().isEmpty());
}

QTEST_MAIN(TestViewerPaging)
#include "test_viewer_paging.moc"
