#pragma once

#include "seer/viewerbase.h"
#include "core/itable_parser.h"
#include "core/pager_state.h"
#include "core/table_source.h"

#include "workers/background_thread.h"

#include <memory>
#include <atomic>
#include <functional>
#include <vector>

#include <QElapsedTimer>
#include <QPointer>

class QStackedLayout;
class QPushButton;
class QSettings;

namespace dtv {
namespace workers {
class SourceWorker;
class CountWorker;
} // namespace workers

namespace ui {
class SearchBar;
class StatusBar;
class PageBar;
class TableRenderer;
class TablePicker;
class ActionRegistry;
} // namespace ui
} // namespace dtv

class DataTableViewer : public ViewerBase {
    Q_OBJECT
    friend class TestViewerPaging;

public:
    explicit DataTableViewer(QWidget *parent = nullptr);
    ~DataTableViewer() override;

    QString name() const override
    {
        return "DataTableViewer";
    }
    QSize getContentSize() const override
    {
        return {960, 600};
    }
    void loadImpl(QBoxLayout *lay_content, QHBoxLayout *lay_ctrlbar) override;
    void updateDPR(qreal r) override;
    void updateTheme(int theme) override;

    void onCopyTriggered() override;
    dtv::ui::ActionRegistry *actionRegistry() const
    {
        return m_actionRegistry.get();
    }

signals:
    void cancelRequested();

private slots:
    void onParseCompleted(std::shared_ptr<const dtv::core::TableParseResult> result,
                          const QString &tableName, int generation);
    void doLoadFile(const QString &path, const QString &tableName);
    void loadSelectedTable(const QString &path, const QString &tableName);

    void cancelSort();
    void onSortClicked(int column);
    void onFirstPageClicked();
    void onPrevPageClicked();
    void onNextPageClicked();
    void onLastPageClicked();
    void onTextViewBtnClicked();

private:
    void failSortRecovery(const QString &error);
    void recoverSort();
    void finishSortRecovery();
    void init();
    void updateTextViewActionEnabled(bool enabled);
    void cancelPending();
    // Ownership ledger for every worker/thread pair this viewer created,
    // including pairs already retired by cancelPending(). A retired pair frees
    // itself asynchronously, so the destructor needs the full list to join
    // threads that are still running.
    void trackWorker(QObject *worker, BackgroundThread *thread);
    void reapWorkers();
    void joinBackgroundWorkers();
    void reapplyStyles();
    QString makeKey(const QString &format, const QString &table) const;
    QString getIniPath() const;
    QSettings &ini();
    void saveCurrentHeaderState();
    int readConfiguredPageSize() const;
    void navigatePage(int64_t targetPage, bool arrivedFromPrev,
                      std::function<void(uint64_t viewGen, uint64_t opGen)> fetchFunc);
    // Reopens m_sourcePath/m_sourceTable on the current worker, choosing the
    // SQLite path or the CSV descriptor from m_isCsv.
    void openCurrentSource(uint64_t viewGen, uint64_t opGen);

    dtv::ui::SearchBar *m_search = nullptr;
    dtv::ui::StatusBar *m_status = nullptr;
    dtv::ui::PageBar *m_pageBar = nullptr;
    dtv::ui::TableRenderer *m_renderer = nullptr;
    dtv::ui::TablePicker *m_picker = nullptr;
    QStackedLayout *m_stack = nullptr;
    QPushButton *m_backBtn = nullptr;
    // QPointer because this button is handed to the host-provided control-bar
    // layout, which may take ownership of it; m_backBtn above is inserted into
    // the plugin's own search layout and is owned by this widget's tree.
    QPointer<QPushButton> m_btnTextView;

    QString m_currentPath;
    int m_generation = 0;
    bool m_isDarkMode = false;
    qreal m_dpr = 1.0;

    // Paged SQLite browsing state
    struct SortState {
        int column;
        bool ascending;
    };
    std::optional<SortState> m_committedSort;
    std::optional<SortState> m_pendingSort;
    bool m_canSort = false;
    bool m_sourceOrderValid = true;
    bool m_sorting = false;
    bool m_recoveringSort = false;
    bool m_sortTotalAuthoritative = false;
    QString m_sourcePath;
    QString m_sourceTable;
    bool m_isPaged = false;
    bool m_isCsv = false;
    // First page of the current table load still owes its one-time UI setup.
    // The stacked widget cannot answer this: a CSV goes straight to the
    // renderer, which is already the current widget, so "is the renderer
    // showing" is true before any page has been delivered.
    bool m_firstPagePending = false;
    bool m_pageFetchInFlight = false;
    bool m_countRequested = false;
    bool m_countFailed = false;
    int m_countCompletedCount = 0;
    std::optional<int> m_pageSizeOverride;
    std::optional<QString> m_iniPathOverride;
    int64_t m_pendingPage = 1;
    bool m_pendingArrivedFromPrev = false;
    dtv::core::PagerState m_pagerState;
    dtv::core::PageToken m_currentToken;
    int m_colCount = 0;
    qint64 m_fileBytes = 0;
    qint64 m_openElapsedMs = 0;
    QElapsedTimer m_pageTimer;

    struct WorkerHandle {
        QPointer<QObject> worker;
        QPointer<BackgroundThread> thread;
    };
    std::vector<WorkerHandle> m_workers;

    std::shared_ptr<std::atomic<uint64_t>> m_viewGen;
    std::shared_ptr<std::atomic<uint64_t>> m_opGen;
    BackgroundThread *m_sourceThread = nullptr;
    BackgroundThread *m_countThread = nullptr;
    dtv::workers::SourceWorker *m_sourceWorker = nullptr;
    dtv::workers::CountWorker *m_countWorker = nullptr;

    mutable QString m_iniPath;
    std::unique_ptr<QSettings> m_ini;
    std::unique_ptr<dtv::ui::ActionRegistry> m_actionRegistry;
};

// Plugin entry point
class DTVPlugin : public QObject, public ViewerPluginInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ViewerPluginInterface_iid FILE "../../bin/plugin.json")
    Q_INTERFACES(ViewerPluginInterface)
public:
    ViewerBase *createViewer(QWidget *parent = nullptr) override
    {
        return new DataTableViewer(parent);
    }
};
