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
#include <QStackedLayout>
#include <QPushButton>
#include <QHeaderView>

class TestViewerPaging : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    void test1MRowDatabaseOpensAndPages();
    void testSmallTableHidesPager();
    void testCsvUnchanged();
    void testBackButtonTeardown();
    void testHeaderStatePreservedAcrossPageTurns();
    void testCountRequestedOnlyOnceOnPageTurns();
    void testPageNavigationFailureRollsBackPageNumber();
    void testLargeTableRowRangeDoesNotOverflow();
    void testClampedLongTextCopyRefetchesFullContent();
    void testEmptyPageNavigationPreservesStateAndToken();

private:
    std::unique_ptr<QTemporaryDir> m_tempDir;
    QString m_1mDbPath;
    QString m_smallDbPath;
    QString m_longTextDbPath;
    QString m_emptyPageDbPath;
    QString m_longTextValue;

    void setupViewer(DataTableViewer &viewer, const QString &path,
                     ViewOptionsPrivate &optsPriv, ViewOptions &opts);
    bool createDatabase(const QString &filePath, int rowCount);
    bool createLongTextDatabase(const QString &filePath, int textLength);
};

bool TestViewerPaging::createDatabase(const QString &filePath, int rowCount) {
    sqlite3 *db = nullptr;
    if (sqlite3_open(filePath.toUtf8().constData(), &db) != SQLITE_OK) {
        return false;
    }
    sqlite3_exec(db, "PRAGMA journal_mode = OFF; PRAGMA synchronous = OFF;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, val REAL);",
                 nullptr, nullptr, nullptr);
    std::string sql =
        "WITH RECURSIVE cnt(x) AS ("
        "  SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < " + std::to_string(rowCount) +
        ") "
        "INSERT INTO items(id, name, val) "
        "SELECT x, 'Item_' || x, (x * 1.5) FROM cnt;";
    char *errmsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errmsg);
    if (errmsg) {
        sqlite3_free(errmsg);
    }
    sqlite3_close(db);
    return rc == SQLITE_OK;
}

bool TestViewerPaging::createLongTextDatabase(const QString &filePath, int textLength) {
    sqlite3 *db = nullptr;
    if (sqlite3_open(filePath.toUtf8().constData(), &db) != SQLITE_OK) {
        return false;
    }
    sqlite3_exec(db, "PRAGMA journal_mode = OFF; PRAGMA synchronous = OFF;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, val REAL);",
                 nullptr, nullptr, nullptr);
    std::string longText(static_cast<size_t>(textLength), 'A');
    std::string sql = "INSERT INTO items(id, name, val) VALUES (1, '" + longText + "', 1.5);";
    char *errmsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errmsg);
    if (errmsg) {
        sqlite3_free(errmsg);
    }
    sqlite3_close(db);
    return rc == SQLITE_OK;
}

void TestViewerPaging::setupViewer(DataTableViewer &viewer, const QString &path,
                                    ViewOptionsPrivate &optsPriv, ViewOptions &opts) {
    // Keep the page size and the settings file inside the test's temp dir so
    // neither the host configuration nor the plugin directory is touched.
    viewer.m_pageSizeOverride = 500;
    viewer.m_iniPathOverride = m_tempDir->filePath("viewer.ini");
    optsPriv.path = path;
    optsPriv.viewer_type = viewer.name();
    optsPriv.theme = 1;
    optsPriv.dpr = 1.0;
    opts.d_ptr = &optsPriv;
    viewer.load(nullptr, &opts);
}

void TestViewerPaging::initTestCase() {
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

void TestViewerPaging::cleanupTestCase() {
    QFile::remove(m_1mDbPath);
    QFile::remove(m_smallDbPath);
    QFile::remove(m_longTextDbPath);
    QFile::remove(m_emptyPageDbPath);
    m_tempDir.reset();
}

void TestViewerPaging::test1MRowDatabaseOpensAndPages() {
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, m_1mDbPath, optsPriv, opts);

    // Initial load displays TablePicker with table list
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_picker, 5000);
    QVERIFY(viewer.m_pageBar->isHidden());

    // Select "items" table
    QElapsedTimer timer;
    timer.start();
    viewer.loadSelectedTable(m_1mDbPath, "items");

    // First page appears promptly (< 3000ms) before COUNT completes
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 1000);
    qint64 firstPageElapsed = timer.elapsed();
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

void TestViewerPaging::testSmallTableHidesPager() {
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, m_smallDbPath, optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_picker, 5000);

    viewer.loadSelectedTable(m_smallDbPath, "items");
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 2000);

    // Wait for count to land
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_pagerState.total.has_value(), 5000);
    QCOMPARE(*viewer.m_pagerState.total, 50LL);

    // Small table (50 <= 500) must hide the page bar
    QVERIFY(viewer.m_pageBar->isHidden());
    QVERIFY(!viewer.m_pageBar->shouldBeVisible());
}

void TestViewerPaging::testCsvUnchanged() {
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, QString(FIXTURES_DIR) + "/valid_basic.csv", optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 5000);

    // CSV/TSV is materialized, pager must be hidden
    QVERIFY(!viewer.m_isPaged);
    QVERIFY(viewer.m_pageBar->isHidden());
    QVERIFY(!viewer.m_renderer->isPagedMode());
}

void TestViewerPaging::testBackButtonTeardown() {
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, m_smallDbPath, optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_picker, 5000);

    viewer.loadSelectedTable(m_smallDbPath, "items");
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 2000);

    // Click back button
    viewer.m_backBtn->click();

    // Must return to picker, pager hidden, paged state reset
    QCOMPARE(viewer.m_stack->currentWidget(), viewer.m_picker);
    QVERIFY(!viewer.m_isPaged);
    QVERIFY(viewer.m_pageBar->isHidden());
    // The previous table's row/column metrics must not linger in the status bar
    QVERIFY(viewer.m_status->text().isEmpty());
    QVERIFY(viewer.m_sourceWorker == nullptr);
    QVERIFY(viewer.m_countWorker == nullptr);
}

void TestViewerPaging::testHeaderStatePreservedAcrossPageTurns() {
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, m_1mDbPath, optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_picker, 5000);

    viewer.loadSelectedTable(m_1mDbPath, "items");
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 2000);

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

void TestViewerPaging::testCountRequestedOnlyOnceOnPageTurns() {
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, m_1mDbPath, optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_picker, 5000);

    viewer.loadSelectedTable(m_1mDbPath, "items");
    QVERIFY(viewer.m_countWorker != nullptr);

    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 2000);

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

void TestViewerPaging::testPageNavigationFailureRollsBackPageNumber() {
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, m_1mDbPath, optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_picker, 5000);

    viewer.loadSelectedTable(m_1mDbPath, "items");
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 2000);

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

void TestViewerPaging::testLargeTableRowRangeDoesNotOverflow() {
    dtv::core::PagerState state;
    state.pageSize = 500;
    state.page = 10000000LL; // 10 millionth page
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

void TestViewerPaging::testClampedLongTextCopyRefetchesFullContent() {
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, m_longTextDbPath, optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_picker, 5000);

    viewer.loadSelectedTable(m_longTextDbPath, "items");
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 1, 2000);

    // The 5000-char name exceeds the 4096-byte page clamp, so the cell must be
    // flagged and its displayed value must be the clamped text.
    QVERIFY(viewer.m_renderer->isCellClamped(0, 1));
    QString clampedDisplay = viewer.m_renderer->model()->index(0, 1).data(Qt::DisplayRole).toString();
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

void TestViewerPaging::testEmptyPageNavigationPreservesStateAndToken() {
    DataTableViewer viewer;
    ViewOptionsPrivate optsPriv;
    ViewOptions opts;
    setupViewer(viewer, m_emptyPageDbPath, optsPriv, opts);
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_picker, 5000);

    viewer.loadSelectedTable(m_emptyPageDbPath, "items");
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_stack->currentWidget() == viewer.m_renderer, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 1, 2000);

    // Go to page 2 (rows 501..1000)
    viewer.onNextPageClicked();
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 2, 2000);
    dtv::core::PageToken keptToken = viewer.m_currentToken;
    QVERIFY(keptToken.valid);

    // Externally delete rows 601..1200 so page 3 no longer has any rows
    sqlite3 *db = nullptr;
    QVERIFY(sqlite3_open(m_emptyPageDbPath.toUtf8().constData(), &db) == SQLITE_OK);
    QVERIFY(sqlite3_exec(db, "DELETE FROM items WHERE id > 600;", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);

    // Next hits the empty page: page number, rows and token must stay on page 2
    viewer.onNextPageClicked();
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_status->text().contains("No more rows exist"), 3000);
    QCOMPARE(viewer.m_pagerState.page, 2LL);
    QCOMPARE(viewer.m_currentToken.firstKey, keptToken.firstKey);
    QCOMPARE(viewer.m_currentToken.lastKey, keptToken.lastKey);
    QVERIFY(viewer.m_currentToken.valid);

    // Fresh COUNT rebuilds the pager bounds: total 600 -> 2 pages, no Next
    QTRY_VERIFY_WITH_TIMEOUT(viewer.m_pagerState.total.has_value() && *viewer.m_pagerState.total == 600LL, 3000);
    QVERIFY(!viewer.m_pagerState.canNext());
    QVERIFY(viewer.m_pagerState.canPrev());

    // Prev still works with the kept token
    viewer.onPrevPageClicked();
    QTRY_VERIFY_WITH_TIMEOUT(!viewer.m_pageFetchInFlight && viewer.m_pagerState.page == 1, 2000);
}

QTEST_MAIN(TestViewerPaging)
#include "test_viewer_paging.moc"
