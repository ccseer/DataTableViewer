#pragma once

#include "seer/viewerbase.h"
#include "core/itable_parser.h"
#include "core/pager_state.h"
#include "core/table_source.h"

#include <memory>
#include <atomic>
#include <functional>

#include <QElapsedTimer>

class QStackedLayout;
class QPushButton;
class BackgroundThread;
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

signals:
    void cancelRequested();

private slots:
    void onParseCompleted(std::shared_ptr<const dtv::core::TableParseResult> result,
                          const QString &tableName, int generation);
    void doLoadFile(const QString &path, const QString &tableName);
    void loadSelectedTable(const QString &path, const QString &tableName);

    void onFirstPageClicked();
    void onPrevPageClicked();
    void onNextPageClicked();
    void onLastPageClicked();

private:
    void init();
    void cancelPending();
    void reapplyStyles();
    QString makeKey(const QString &format, const QString &table) const;
    QString getIniPath() const;
    QSettings &ini();
    void saveCurrentHeaderState();
    int readConfiguredPageSize() const;
    void navigatePage(int64_t targetPage, bool arrivedFromPrev,
                      std::function<void(uint64_t viewGen, uint64_t opGen)> fetchFunc);

    dtv::ui::SearchBar *m_search = nullptr;
    dtv::ui::StatusBar *m_status = nullptr;
    dtv::ui::PageBar *m_pageBar = nullptr;
    dtv::ui::TableRenderer *m_renderer = nullptr;
    dtv::ui::TablePicker *m_picker = nullptr;
    QStackedLayout *m_stack = nullptr;
    QPushButton *m_backBtn = nullptr;

    QString m_currentPath;
    int m_generation = 0;
    bool m_sqliteAvailable = false;
    bool m_isDarkMode = false;
    qreal m_dpr = 1.0;

    // Paged SQLite browsing state
    bool m_isPaged = false;
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

    std::shared_ptr<std::atomic<uint64_t>> m_viewGen;
    std::shared_ptr<std::atomic<uint64_t>> m_opGen;
    BackgroundThread *m_sourceThread = nullptr;
    BackgroundThread *m_countThread = nullptr;
    dtv::workers::SourceWorker *m_sourceWorker = nullptr;
    dtv::workers::CountWorker *m_countWorker = nullptr;

    mutable QString m_iniPath;
    std::unique_ptr<QSettings> m_ini;
    bool m_iniWriteWarned = false;
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
