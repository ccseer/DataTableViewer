#include "data_table_viewer.h"
#include "action_registry.h"
#include "search_bar.h"
#include "status_bar.h"
#include "page_bar.h"
#include "table_renderer.h"
#include "table_picker.h"
#include "style_assets.h"
#include "core/parser_registry.h"
#include "core/pager_state.h"
#include "workers/table_worker.h"
#include "workers/source_worker.h"
#include "workers/count_worker.h"
#include "workers/background_thread.h"
#include "seer/viewerhelper.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedLayout>
#include <QPushButton>
#include <QLineEdit>
#include <QFileInfo>
#include <QPointer>
#include <QSettings>
#include <QElapsedTimer>
#include <QDebug>
#include <QEvent>
#include <QHeaderView>
#include <QCoreApplication>
#include <algorithm>

#define qprintt qDebug() << "[DataTableViewer]"

namespace {
static bool s_iniWriteWarned = false;

bool isSqliteExtension(const QString &suffix)
{
    const QString ext = suffix.toLower();
    return ext == "sqlite" || ext == "sqlite3" || ext == "db" || ext == "db3" || ext == "sl3";
}
bool isCsvExtension(const QString &suffix)
{
    const QString ext = suffix.toLower();
    return ext == "csv" || ext == "tsv";
}
} // namespace

DataTableViewer::DataTableViewer(QWidget *parent) : ViewerBase(parent)
{
    qprintt << this;
}

DataTableViewer::~DataTableViewer()
{
    // This viewer lives in a DLL the host may unload as soon as the last
    // viewer instance is gone. Any background thread still executing code
    // inside that DLL would fetch instructions from unmapped memory, so every
    // thread has to be joined before this destructor returns. cancelPending()
    // stops active workers asynchronously; joinBackgroundWorkers() joins all
    // created threads and reclaims their objects.
    cancelPending();
    joinBackgroundWorkers();
    qprintt << "~" << this;
}

void DataTableViewer::init()
{
    if(m_renderer) {
        return;
    }

    qRegisterMetaType<std::shared_ptr<const dtv::core::TableParseResult>>(
        "std::shared_ptr<const dtv::core::TableParseResult>");
    dtv::workers::registerWorkerMetatypes();
    dtv::core::ParserRegistry::instance().registerBuiltinParsers();

    // 1. Create UI components
    m_search = new dtv::ui::SearchBar(this);
    m_status = new dtv::ui::StatusBar(this);
    m_pageBar = new dtv::ui::PageBar(this);
    m_pageBar->hide();
    m_renderer = new dtv::ui::TableRenderer(this);
    m_picker = new dtv::ui::TablePicker(this);

    // 2. Setup library paths
    const QString path = seer::getDLLPath();
    if(!path.isEmpty()) {
        QCoreApplication::addLibraryPath(path);
    } else {
        qprintt << "get DLL path failed" << path;
    }

    m_stack = new QStackedLayout;
    m_stack->addWidget(m_renderer);
    m_stack->addWidget(m_picker);

    m_backBtn = new QPushButton(this);
    m_backBtn->hide();
    m_backBtn->setFixedSize(30, 30);
    m_backBtn->setCursor(Qt::PointingHandCursor);
    m_backBtn->setToolTip("Back to table list");

    connect(m_backBtn, &QPushButton::clicked, this, [this] {
        cancelPending();
        m_renderer->clear();
        m_renderer->setStateKey({});
        m_renderer->setPagedMode(false);
        m_search->setPagedMode(false);
        m_status->setPagedMode(false);
        m_pageBar->hide();
        m_stack->setCurrentWidget(m_picker);
        m_backBtn->hide();
        m_search->hide();
        m_status->clear();
    });

    connect(m_picker, &dtv::ui::TablePicker::tableSelected, this, [this](const QString &name) {
        loadSelectedTable(m_currentPath, name);
    });

    connect(m_search, &dtv::ui::SearchBar::filterChanged, this, [this](const QString &text) {
        m_renderer->setFilter(text, -1);
    });

    connect(m_renderer, &dtv::ui::TableRenderer::requestFilter, this, [this](const QString &text) {
        m_search->setText(text);
    });

    connect(m_renderer, &dtv::ui::TableRenderer::currentItemChanged, this,
            [this](const QString &header, const QString &value, int modelRow, int /*modelCol*/) {
                if(header.isEmpty() || modelRow < 0) {
                    m_status->restoreInfo();
                    return;
                }

                int pageRow = modelRow + 1;
                int pageSize =
                    m_pagerState.pageSize > 0 ? m_pagerState.pageSize : m_renderer->rowCount();

                QString prefix;
                if(m_isPaged && m_pagerState.pageSize > 0) {
                    int64_t globalRow =
                        dtv::core::firstRowOnPage(m_pagerState.page, m_pagerState.pageSize) +
                        modelRow;
                    if(m_pagerState.total.has_value()) {
                        prefix = QString("[Row %1/%2, Global #%L3/%L4] ")
                                     .arg(pageRow)
                                     .arg(pageSize)
                                     .arg(globalRow)
                                     .arg(*m_pagerState.total);
                    } else {
                        prefix = QString("[Row %1/%2, Global #%L3] ")
                                     .arg(pageRow)
                                     .arg(pageSize)
                                     .arg(globalRow);
                    }
                } else {
                    prefix = QString("[Row %1/%2] ").arg(pageRow).arg(pageSize);
                }

                m_status->setValueText(QString("%1%2 : %3").arg(prefix, header, value));
            });

    connect(m_renderer, &dtv::ui::TableRenderer::filterCountChanged, this, [this](int matches) {
        m_status->setFilterMatchCount(matches, !m_search->text().isEmpty());
    });

    connect(m_renderer->horizontalHeader(), &QHeaderView::sectionClicked, this,
            &DataTableViewer::onSortClicked);
    connect(m_status, &dtv::ui::StatusBar::cancelSortRequested, this, &DataTableViewer::cancelSort);

    // Pager navigation button connections
    connect(m_pageBar, &dtv::ui::PageBar::firstClicked, this, &DataTableViewer::onFirstPageClicked);
    connect(m_pageBar, &dtv::ui::PageBar::prevClicked, this, &DataTableViewer::onPrevPageClicked);
    connect(m_pageBar, &dtv::ui::PageBar::nextClicked, this, &DataTableViewer::onNextPageClicked);
    connect(m_pageBar, &dtv::ui::PageBar::lastClicked, this, &DataTableViewer::onLastPageClicked);

    // Keyboard navigation connections (PageUp/PageDown on table view)
    connect(m_renderer, &dtv::ui::TableRenderer::pageUpRequested, this,
            &DataTableViewer::onPrevPageClicked);
    connect(m_renderer, &dtv::ui::TableRenderer::pageDownRequested, this,
            &DataTableViewer::onNextPageClicked);

    connect(
        m_renderer, &dtv::ui::TableRenderer::copyRefetchIncomplete, this, [this](int failedRows) {
            if(failedRows < 0) {
                m_status->setValueText(tr("Copy failed: %1 MiB payload budget exceeded")
                                           .arg(static_cast<qlonglong>(
                                               dtv::core::kMaxCopyBudgetBytes / (1024 * 1024))));
            } else {
                m_status->setValueText(
                    tr("Copy incomplete: %1 truncated row(s) could not be refreshed")
                        .arg(failedRows));
            }
        });

    connect(m_renderer, &dtv::ui::TableRenderer::refetchRowsRequested, this,
            [this](uint64_t copyRequestId, bool isMarkdown, const auto &rowKeys) {
                Q_UNUSED(isMarkdown);
                // The generation counters exist only while a paged source is
                // loaded; asking for clamped cells before that must fall back
                // instead of dereferencing an empty shared_ptr.
                if(m_sourceWorker && m_isPaged && m_viewGen) {
                    QMetaObject::invokeMethod(m_sourceWorker, "refetchRows", Qt::QueuedConnection,
                                              Q_ARG(uint64_t, m_viewGen->load()),
                                              Q_ARG(uint64_t, copyRequestId),
                                              Q_ARG(RefetchRowKeysList, rowKeys));
                } else {
                    // Fallback: no worker available or not paged, complete copy with truncated cells
                    m_renderer->onRefetchRowsCompleted(copyRequestId, {});
                }
            });

    m_actionRegistry = std::make_unique<dtv::ui::ActionRegistry>(this);
    ini().sync();
    m_actionRegistry->loadShortcuts(ini());

    m_actionRegistry->registerAction("DataTableViewer.find", tr("Find"), QKeySequence::Find,
                                     [this] {
                                         m_search->setFocus();
                                         m_search->selectAll();
                                     });

    auto *copyAction = m_actionRegistry->registerAction("DataTableViewer.copy", tr("Copy"),
                                                        QKeySequence::Copy, [this] {
                                                            onCopyTriggered();
                                                        });

    m_actionRegistry->registerAction("DataTableViewer.viewText", tr("View in Text viewer"),
                                     QKeySequence("Ctrl+Alt+T"), [this] {
                                         onTextViewBtnClicked();
                                     });

    m_actionRegistry->registerAction("DataTableViewer.pageFirst", tr("First page"),
                                     QKeySequence(Qt::ControlModifier | Qt::Key_Home), [this] {
                                         onFirstPageClicked();
                                     });

    m_actionRegistry->registerAction("DataTableViewer.pagePrev", tr("Previous page"),
                                     QKeySequence(Qt::ControlModifier | Qt::Key_PageUp), [this] {
                                         onPrevPageClicked();
                                     });

    m_actionRegistry->registerAction("DataTableViewer.pageNext", tr("Next page"),
                                     QKeySequence(Qt::ControlModifier | Qt::Key_PageDown), [this] {
                                         onNextPageClicked();
                                     });

    m_actionRegistry->registerAction("DataTableViewer.pageLast", tr("Last page"),
                                     QKeySequence(Qt::ControlModifier | Qt::Key_End), [this] {
                                         onLastPageClicked();
                                     });

    m_pageBar->setShortcutHints(m_actionRegistry->shortcutNativeText("DataTableViewer.pageFirst"),
                                m_actionRegistry->shortcutNativeText("DataTableViewer.pagePrev"),
                                m_actionRegistry->shortcutNativeText("DataTableViewer.pageNext"),
                                m_actionRegistry->shortcutNativeText("DataTableViewer.pageLast"));

    // Seed default configuration into DataTableViewer.ini if missing so users can discover and edit it
    if(!ini().contains("page_rows") && !ini().contains("DataTableViewer/page_rows")) {
        ini().setValue("page_rows", 500);
    }
    bool showRowIndex = ini().value("row_index_b", true).toBool();
    if(!ini().contains("row_index_b")) {
        ini().setValue("row_index_b", true);
    }
    m_actionRegistry->saveDefaultsIfMissing(ini());
    ini().sync();
    if(!s_iniWriteWarned && ini().status() != QSettings::NoError) {
        s_iniWriteWarned = true;
        qprintt << "failed to write initial configuration to" << getIniPath();
    }

    m_renderer->setCopyAction(copyAction);
    m_renderer->setShowRowIndex(showRowIndex);
    updateTextViewActionEnabled(!m_currentPath.isEmpty());
}

void DataTableViewer::updateTextViewActionEnabled(bool enabled)
{
    if(m_btnTextView) {
        m_btnTextView->setEnabled(enabled);
    }
    if(m_actionRegistry) {
        if(auto *act = m_actionRegistry->action("DataTableViewer.viewText")) {
            act->setEnabled(enabled);
        }
    }
}

void DataTableViewer::loadImpl(QBoxLayout *lay_content, QHBoxLayout *lay_ctrlbar)
{
    init();

    if(lay_content && lay_content->indexOf(m_search) == -1) {
        lay_content->setContentsMargins(0, 0, 0, 0);
        lay_content->setSpacing(qRound(6 * m_dpr));
        lay_content->addWidget(m_search);
        lay_content->addLayout(m_stack, 1);
        lay_content->addWidget(m_pageBar);
        lay_content->addWidget(m_status);
    }

    if(auto *slay = qobject_cast<QHBoxLayout *>(m_search->layout())) {
        if(slay->indexOf(m_backBtn) == -1) {
            slay->insertWidget(0, m_backBtn);
        }
    }

    // The button is created unconditionally so a viewer loaded without a
    // control bar still owns it and its action stays reachable; only the
    // attachment to the host-provided layout is conditional.
    if(!m_btnTextView) {
        m_btnTextView = new QPushButton(this);
        m_btnTextView->setObjectName("textViewBtn");
        m_btnTextView->setFlat(true);
        m_btnTextView->setFocusPolicy(Qt::NoFocus);
        m_btnTextView->setCursor(Qt::PointingHandCursor);
        connect(m_btnTextView, &QPushButton::clicked, this, &DataTableViewer::onTextViewBtnClicked);
    }
    const QString shortcutHint =
        m_actionRegistry ? m_actionRegistry->shortcutNativeText("DataTableViewer.viewText")
                         : QString();
    if(!shortcutHint.isEmpty()) {
        m_btnTextView->setToolTip(tr("View in Text viewer (%1)").arg(shortcutHint));
    } else {
        m_btnTextView->setToolTip(tr("View in Text viewer"));
    }
    if(lay_ctrlbar && lay_ctrlbar->indexOf(m_btnTextView) == -1) {
        lay_ctrlbar->addStretch();
        lay_ctrlbar->addWidget(m_btnTextView);
        m_btnTextView->show();
    } else if(!lay_ctrlbar) {
        // Without a control bar the button is a child of this viewer but is in
        // no layout, so it would otherwise be shown at (0,0) on top of the
        // search bar as soon as the host shows the viewer. The action stays
        // reachable either way.
        m_btnTextView->hide();
    }

    m_currentPath = options()->path();
    updateTextViewActionEnabled(!m_currentPath.isEmpty());

    if(options()) {
        QVariant minSzVar = options()->property(ViewOptionsKeys::kKeySizeMin);
        if(minSzVar.isValid() && minSzVar.canConvert<QSize>()) {
            QSize minSz = minSzVar.toSize();
            if(minSz.isValid() && minSz.width() > 0 && minSz.height() > 0) {
                setMinimumSize(minSz);
            }
        }
        QVariant maxSzVar = options()->property(ViewOptionsKeys::kKeySizeMax);
        if(maxSzVar.isValid() && maxSzVar.canConvert<QSize>()) {
            QSize maxSz = maxSzVar.toSize();
            if(maxSz.isValid() && maxSz.width() > 0 && maxSz.height() > 0) {
                setMaximumSize(maxSz);
            }
        }
    }

    updateTheme(options()->theme());
    updateDPR(options()->dpr());

    if(!m_currentPath.isEmpty()) {
        doLoadFile(m_currentPath, "");
    }
}

void DataTableViewer::updateDPR(qreal r)
{
    m_dpr = r;
    if(m_search)
        m_search->updateDPR(r);
    if(m_status)
        m_status->updateTheme(m_isDarkMode, r);
    if(m_pageBar)
        m_pageBar->updateTheme(m_isDarkMode, r);
    if(m_picker)
        m_picker->updateTheme(m_isDarkMode, r);
    if(m_renderer)
        m_renderer->updateTheme(m_isDarkMode, r);

    if(layout()) {
        layout()->setSpacing(qRound(6 * r));
    }

    reapplyStyles();
}

void DataTableViewer::updateTheme(int theme)
{
    m_isDarkMode = (theme == 1);
    if(m_search)
        m_search->updateTheme(m_isDarkMode);
    if(m_status)
        m_status->updateTheme(m_isDarkMode, m_dpr);
    if(m_pageBar)
        m_pageBar->updateTheme(m_isDarkMode, m_dpr);
    if(m_picker)
        m_picker->updateTheme(m_isDarkMode, m_dpr);
    if(m_renderer)
        m_renderer->updateTheme(m_isDarkMode, m_dpr);

    reapplyStyles();
}

void DataTableViewer::reapplyStyles()
{
    using namespace dtv::ui;
    if(!m_search || !m_status || !m_backBtn)
        return;

    const char *surface = m_isDarkMode ? Colors::DarkSurface : Colors::LightSurface;
    const char *border = m_isDarkMode ? Colors::DarkBorder : Colors::LightBorder;
    const char *input = m_isDarkMode ? Colors::DarkInput : Colors::LightInput;
    const char *text = m_isDarkMode ? Colors::DarkText : Colors::LightText;
    const char *accent = Colors::Accent;

    m_search->setStyleSheet(QString(g_qss_top_bar)
                                .arg(surface, border, input, text, accent)
                                .arg(qRound(6 * m_dpr))
                                .arg(qRound(4 * m_dpr))
                                .arg(qRound(8 * m_dpr)));

    m_status->setStyleSheet(QString(g_qss_bottom_bar).arg(surface, border));

    QColor iconColor(m_isDarkMode ? Colors::DarkText : Colors::LightText);
    int iconSize = qRound(18 * m_dpr);
    m_backBtn->setIcon(createIcon(g_svg_arrow_back, iconColor, iconSize));
    m_backBtn->setIconSize(QSize(iconSize, iconSize));
    m_backBtn->setFixedSize(qRound(30 * m_dpr), qRound(30 * m_dpr));
    m_backBtn->setStyleSheet("QPushButton { border: none; background: transparent; "
                             "border-radius: 5px; }"
                             "QPushButton:hover { background-color: rgba(128, 128, 128, 36); }"
                             "QPushButton:pressed { background-color: rgba(128, 128, 128, 58); }");

    if(m_btnTextView) {
        constexpr int kCtrlbarBtnSz = 30;
        constexpr int kCtrlbarBtnIconSz = 24;
        int iconPixelSize = qRound(kCtrlbarBtnIconSz * m_dpr);
        m_btnTextView->setFixedSize(qRound(kCtrlbarBtnSz * m_dpr), qRound(kCtrlbarBtnSz * m_dpr));
        m_btnTextView->setIconSize(QSize(iconPixelSize, iconPixelSize));
        m_btnTextView->setIcon(createMultiStateIcon(g_svg_article, iconColor, iconPixelSize));
        m_btnTextView->setStyleSheet(
            QString(
                "QPushButton#textViewBtn { border: none; background: transparent; border-radius: "
                "%1px; }"
                "QPushButton#textViewBtn:hover { background-color: rgba(128, 128, 128, 36); }"
                "QPushButton#textViewBtn:pressed { background-color: rgba(128, 128, 128, 58); }")
                .arg(qRound(4 * m_dpr)));
    }
}

void DataTableViewer::trackWorker(QObject *worker, BackgroundThread *thread)
{
    m_workers.push_back(WorkerHandle{worker, thread});
}

void DataTableViewer::reapWorkers()
{
    m_workers.erase(std::remove_if(m_workers.begin(), m_workers.end(),
                                   [](const WorkerHandle &h) {
                                       return h.worker.isNull() && h.thread.isNull();
                                   }),
                    m_workers.end());
}

void DataTableViewer::joinBackgroundWorkers()
{
    // Phase 1: Disconnect and interrupt all workers so their destruction
    // on the background thread cannot trigger thread->deleteLater() or
    // notify this viewer. Tell all threads to quit.
    for(auto &handle : m_workers) {
        if(!handle.worker.isNull()) {
            QObject::disconnect(handle.worker.data(), &QObject::destroyed, nullptr, nullptr);
            handle.worker->disconnect();
            if(auto *source = qobject_cast<dtv::workers::SourceWorker *>(handle.worker.data())) {
                source->interrupt();
            } else if(auto *count =
                          qobject_cast<dtv::workers::CountWorker *>(handle.worker.data())) {
                count->interrupt();
            }
        }
        if(!handle.thread.isNull()) {
            handle.thread->requestInterruption();
            handle.thread->quit();
        }
    }

    // Phase 2: Wait for all background threads to stop and join.
    for(auto &handle : m_workers) {
        if(!handle.thread.isNull()) {
            handle.thread->wait();
        }
    }

    // Phase 3: All background threads have terminated. Purge any posted
    // deferred delete events and reclaim any surviving objects.
    // Invariant: worker is deleted before thread so that any potential
    // deferred deletion cascades are caught before thread disposal.
    for(auto &handle : m_workers) {
        if(!handle.worker.isNull()) {
            QCoreApplication::removePostedEvents(handle.worker.data());
            delete handle.worker.data();
        }
        if(!handle.thread.isNull()) {
            QCoreApplication::removePostedEvents(handle.thread.data());
            delete handle.thread.data();
        }
    }
    m_workers.clear();
    m_sourceWorker = nullptr;
    m_sourceThread = nullptr;
    m_countWorker = nullptr;
    m_countThread = nullptr;
}

void DataTableViewer::cancelPending()
{
    saveCurrentHeaderState();

    emit cancelRequested();

    // Every tracked thread belongs to a load this call retires. Asking them to
    // interrupt lets a worker stop between slices instead of finishing work
    // whose result is already discarded.
    for(auto &handle : m_workers) {
        if(!handle.thread.isNull()) {
            handle.thread->requestInterruption();
        }
    }

    m_generation++;
    if(m_viewGen) {
        m_viewGen->fetch_add(1);
    }
    if(m_opGen) {
        m_opGen->fetch_add(1);
    }

    if(m_sourceWorker) {
        m_sourceWorker->interrupt();
        QMetaObject::invokeMethod(m_sourceWorker, "shutdown", Qt::QueuedConnection);
        m_sourceWorker = nullptr;
        m_sourceThread = nullptr;
    }

    if(m_countWorker) {
        m_countWorker->interrupt();
        QMetaObject::invokeMethod(m_countWorker, "shutdown", Qt::QueuedConnection);
        m_countWorker = nullptr;
        m_countThread = nullptr;
    }

    m_isPaged = false;
    m_isCsv = false;
    m_firstPagePending = false;
    if(m_backBtn)
        m_backBtn->hide();
    m_sorting = false;
    m_recoveringSort = false;
    m_canSort = false;
    m_sourceOrderValid = true;
    m_sortTotalAuthoritative = false;
    m_committedSort.reset();
    m_pendingSort.reset();
    if(m_status)
        m_status->setSorting(false);
    m_pageFetchInFlight = false;
    m_countRequested = false;
    m_countFailed = false;
    m_pendingPage = 1;
    m_pendingArrivedFromPrev = false;
    // The pager belongs to the table being retired. Keeping it would hand the
    // next load a foreign total and suppress that load's own COUNT.
    m_pagerState = dtv::core::PagerState{};
    m_currentToken = dtv::core::PageToken{};
    m_colCount = 0;

    // The retired pairs above free themselves asynchronously; drop the ones
    // that already did so the ledger cannot grow across file switches.
    reapWorkers();
}

QSettings &DataTableViewer::ini()
{
    if(!m_ini) {
        m_ini = std::make_unique<QSettings>(getIniPath(), QSettings::IniFormat);
    }
    return *m_ini;
}

void DataTableViewer::saveCurrentHeaderState()
{
    if(!m_renderer) {
        return;
    }
    m_renderer->saveHeaderState(ini());
    ini().sync();
    // A plugin installed under a read-only directory loses the header state
    // silently unless the failure is surfaced once.
    if(!s_iniWriteWarned && ini().status() != QSettings::NoError) {
        s_iniWriteWarned = true;
        qprintt << "failed to write header state to" << getIniPath();
    }
}

QString DataTableViewer::getIniPath() const
{
    if(m_iniPathOverride.has_value()) {
        return *m_iniPathOverride;
    }
    if(m_iniPath.isEmpty()) {
        const QString filename = name() + ".ini";
        QString dir = seer::getDLLPath();
        if(dir.isEmpty()) {
            dir = QCoreApplication::applicationDirPath();
        }
        dir.replace("\\", "/");
        if(!dir.endsWith("/")) {
            dir.append("/");
        }
        m_iniPath = dir + filename;
    }
    return m_iniPath;
}

int DataTableViewer::readConfiguredPageSize() const
{
    if(m_pageSizeOverride.has_value()) {
        return dtv::core::normalizePageRows(*m_pageSizeOverride);
    }
    QSettings &settings = const_cast<DataTableViewer *>(this)->ini();
    QVariant val = settings.value("page_rows");
    if(!val.isValid() || val.isNull()) {
        val = settings.value("DataTableViewer/page_rows");
    }
    if(val.isValid() && !val.isNull()) {
        bool ok = false;
        int rows = val.toInt(&ok);
        if(ok) {
            return dtv::core::normalizePageRows(rows);
        }
    }
    return 500;
}

void DataTableViewer::doLoadFile(const QString &path, const QString &tableName)
{
    cancelPending();

    m_sourcePath = path;
    updateTextViewActionEnabled(!path.isEmpty());

    m_renderer->clear();
    m_renderer->setStateKey({});
    m_search->clear();
    m_search->setPagedMode(false);
    m_status->showLoading();
    m_search->setEnabled(false);
    m_pageBar->hide();

    QFileInfo info(path);
    qprintt << "doLoadFile:" << path << "table:" << tableName << "suffix:" << info.suffix();

    // Direct table request on SQLite goes to paged loader
    if(isSqliteExtension(info.suffix()) && !tableName.isEmpty()) {
        loadSelectedTable(path, tableName);
        return;
    }

    // All CSV and TSV files go to the paged loader (CsvFileSource + SourceWorker).
    // The byte-reading TableWorker path and CsvParser no longer serve production
    // CSV/TSV loading: nothing falls back to them when the paged source fails to
    // open, and they now exist for the parser unit tests and for the non-CSV
    // extensions still registered in ParserRegistry.
    if(isCsvExtension(info.suffix())) {
        loadSelectedTable(path, "");
        return;
    }

    auto parser = dtv::core::ParserRegistry::instance().createParser(info.suffix().toStdString());
    if(!parser) {
        qprintt << "Error: No parser found for extension:" << info.suffix();
        m_status->hideLoading();
        m_status->setValueText("No parser found for extension: " + info.suffix());
        emit sigCommand(VCT_StateChange, VCV_Error);
        return;
    }

    auto *thread = new BackgroundThread;
    auto *worker = new dtv::workers::TableWorker(std::move(parser), path, tableName, m_generation);

    // cancelPending() interrupts every tracked thread, so this path needs no
    // per-load interruption connection.
    connect(worker, &QObject::destroyed, thread, &QObject::deleteLater);

    connect(thread, &QThread::started, worker, &dtv::workers::TableWorker::doParse);
    connect(worker, &dtv::workers::TableWorker::parseCompleted, this,
            [this](auto result, QString tName, int wgen) {
                if(wgen != m_generation)
                    return;
                onParseCompleted(result, tName, wgen);
            });

    worker->moveToThread(thread);
    trackWorker(worker, thread);
    thread->start();
}

void DataTableViewer::loadSelectedTable(const QString &path, const QString &tableName)
{
    cancelPending();

    m_isPaged = true;
    m_firstPagePending = true;
    m_sourcePath = path;
    updateTextViewActionEnabled(!path.isEmpty());
    m_sourceTable = tableName;
    m_countRequested = false;
    m_pendingPage = 1;
    m_pendingArrivedFromPrev = false;
    m_renderer->clear();
    m_renderer->setStateKey({});
    m_search->clear();
    m_status->showLoading();
    m_search->setEnabled(false);
    m_pageBar->hide();
    if(m_backBtn)
        m_backBtn->hide();

    QFileInfo info(path);
    m_fileBytes = info.size();
    m_isCsv = isCsvExtension(info.suffix());

    // Resolved once per load: every page of the same file reports the same
    // format, credit and header-state key.
    const bool isTsv = m_isCsv && info.suffix().compare("tsv", Qt::CaseInsensitive) == 0;
    const QString formatName = m_isCsv ? (isTsv ? "TSV" : "CSV") : "SQLite";
    const QString libraryCredit = m_isCsv ? "(built-in RFC 4180 parser)" : "SQLite 3";
    // Header state is keyed by the format name the pre-paging CSV path used:
    // CsvParser reported "CSV/TSV" for auto-detected .csv files and "TSV" for
    // .tsv files. Reusing those keys keeps the column layouts users saved
    // before CSV paging shipped.
    const QString stateKey =
        !m_isCsv ? makeKey("SQLite", tableName) : (isTsv ? QString("TSV") : QString("CSV/TSV"));

    m_openElapsedMs = 0;
    m_pageTimer.start();

    m_viewGen = std::make_shared<std::atomic<uint64_t>>(m_generation);
    m_opGen = std::make_shared<std::atomic<uint64_t>>(1);

    m_sourceThread = new BackgroundThread;
    m_sourceWorker = new dtv::workers::SourceWorker(m_viewGen, m_opGen);
    m_sourceWorker->moveToThread(m_sourceThread);
    connect(m_sourceWorker, &QObject::destroyed, m_sourceThread, &QObject::deleteLater);
    trackWorker(m_sourceWorker, m_sourceThread);

    if(!m_isCsv) {
        m_countThread = new BackgroundThread;
        m_countWorker = new dtv::workers::CountWorker(m_viewGen);
        m_countWorker->moveToThread(m_countThread);
        connect(m_countWorker, &QObject::destroyed, m_countThread, &QObject::deleteLater);
        trackWorker(m_countWorker, m_countThread);
    }

    connect(m_sourceWorker, &dtv::workers::SourceWorker::indexProgress, this,
            [this](uint64_t viewGen, qint64 totalRows, bool isComplete, const QString &error) {
                if(viewGen != static_cast<uint64_t>(m_generation) || !m_isPaged || !m_isCsv)
                    return;

                if(!error.isEmpty()) {
                    // Indexing is terminal on error: no further slice follows,
                    // so the spinner must stop and the reason must be shown.
                    m_status->setIndexingFailed(error);
                    return;
                }

                if(isComplete) {
                    m_pagerState.total = totalRows;
                    m_pagerState.hasMore = (totalRows > m_pagerState.page * m_pagerState.pageSize);
                    m_status->updatePagedTotal(totalRows);
                    if(!m_firstPagePending) {
                        // setPagerState applies PageBar::shouldBeVisible(), the
                        // single owner of the pager hide/show rule.
                        m_pageBar->setPagerState(m_pagerState);
                    }
                } else {
                    m_status->setIndexingProgress(totalRows);
                }
            });

    connect(m_sourceWorker, &dtv::workers::SourceWorker::openCompleted, this,
            [this](uint64_t viewGen, uint64_t opGen, bool ok, const QString &error,
                   const std::vector<dtv::core::ColumnMeta> &columns, bool canSort) {
                if(viewGen != static_cast<uint64_t>(m_generation) || !m_isPaged ||
                   opGen != m_opGen->load())
                    return;

                m_canSort = canSort;
                if(m_recoveringSort) {
                    if(!ok) {
                        failSortRecovery(error);
                        return;
                    }
                    if(m_committedSort) {
                        auto state = *m_committedSort;
                        QMetaObject::invokeMethod(
                            m_sourceWorker,
                            [worker = m_sourceWorker, viewGen, opGen, state] {
                                worker->sort(viewGen, opGen, state.column, state.ascending);
                            },
                            Qt::QueuedConnection);
                    } else {
                        finishSortRecovery();
                    }
                    return;
                }

                if(!ok) {
                    cancelPending();
                    m_status->clear();
                    m_status->setValueText("Error: " + error);
                    emit sigCommand(VCT_StateChange, VCV_Error);
                    return;
                }

                m_colCount = static_cast<int>(columns.size());
                int pageSize = readConfiguredPageSize();
                auto savedTotal = m_pagerState.total;
                m_pagerState = dtv::core::PagerState{};
                m_pagerState.total = savedTotal;
                m_pagerState.pageSize = pageSize;
                m_pagerState.page = 1;
                if(savedTotal.has_value()) {
                    m_pagerState.hasMore = (*savedTotal > pageSize);
                }
                m_pendingPage = 1;
                m_pendingArrivedFromPrev = false;
                m_pageFetchInFlight = true;
                m_pageBar->setBusy(true);

                QMetaObject::invokeMethod(m_sourceWorker, "first", Qt::QueuedConnection,
                                          Q_ARG(uint64_t, viewGen), Q_ARG(uint64_t, opGen),
                                          Q_ARG(int, pageSize));
            });

    connect(
        m_sourceWorker, &dtv::workers::SourceWorker::pageReady, this,
        [this, path, tableName, formatName, libraryCredit, stateKey](
            uint64_t viewGen, uint64_t opGen, std::shared_ptr<const dtv::core::PageResult> result) {
            if(viewGen != static_cast<uint64_t>(m_generation) ||
               (m_opGen && opGen != m_opGen->load()) || !m_isPaged)
                return;

            m_pageFetchInFlight = false;
            m_pageBar->setBusy(false);

            if(!result->ok) {
                if(m_sorting) {
                    recoverSort();
                    return;
                }
                m_status->setValueText("Error: " + QString::fromStdString(result->error));
                m_pendingPage = m_pagerState.page;
                emit sigCommand(VCT_StateChange, VCV_Error);
                return;
            }

            if(!result->data) {
                if(m_sorting) {
                    recoverSort();
                    return;
                }
                m_status->setValueText(tr("Error: Empty page result payload"));
                m_pendingPage = m_pagerState.page;
                emit sigCommand(VCT_StateChange, VCV_Error);
                return;
            }

            const bool isInitialLoad = m_firstPagePending;

            if(!isInitialLoad && !m_sorting && result->data->rows.empty()) {
                // The requested page vanished because data was modified or
                // deleted externally. Keep the committed page, its rows and
                // its token untouched so navigation can continue, then let a
                // fresh COUNT rebuild the pager bounds.
                m_pagerState.hasMore = false;
                m_pageBar->setPagerState(m_pagerState);
                m_status->setValueText(tr("No more rows exist (data modified externally)"));

                if(!m_countFailed && m_countWorker) {
                    m_sortTotalAuthoritative = false;
                    m_countRequested = true;
                    QMetaObject::invokeMethod(m_countWorker, "count", Qt::QueuedConnection,
                                              Q_ARG(uint64_t, viewGen), Q_ARG(QString, path),
                                              Q_ARG(QString, tableName));
                }
                return;
            }

            // Wall clock for the page that just arrived, restarted per navigation request.
            m_openElapsedMs = m_pageTimer.elapsed();
            m_pagerState.page = m_pendingPage;
            m_pagerState.arrivedFromPrev = m_pendingArrivedFromPrev;
            if(m_pagerState.total.has_value()) {
                m_pagerState.hasMore =
                    (*m_pagerState.total > m_pagerState.page * m_pagerState.pageSize);
            } else {
                m_pagerState.hasMore = result->hasMore;
            }
            m_currentToken = result->token;

            int64_t rowCount = static_cast<int64_t>(result->data->rows.size());
            int64_t firstRow =
                rowCount > 0 ? dtv::core::firstRowOnPage(m_pagerState.page, m_pagerState.pageSize)
                             : 0;
            int64_t rowOffset = firstRow > 0 ? (firstRow - 1) : 0;

            m_renderer->setPageData(result->data, result->keys, result->clamped, rowOffset);
            if(m_sorting && !m_recoveringSort) {
                m_committedSort = m_pendingSort;
                m_pendingSort.reset();
                finishSortRecovery();
            }

            if(isInitialLoad) {
                m_firstPagePending = false;
                m_renderer->setStateKey(stateKey);
                m_renderer->restoreHeaderState(ini());
                m_renderer->horizontalHeader()->setSortIndicatorShown(false);
                m_stack->setCurrentWidget(m_renderer);
                m_search->show();
                m_search->setEnabled(true);
                if(m_backBtn)
                    m_backBtn->setVisible(!m_isCsv);
                m_renderer->setPagedMode(true);
                m_search->setPagedMode(true);
                m_status->setPagedMode(true);
            }

            int64_t lastRow = rowCount > 0 ? firstRow + rowCount - 1 : 0;

            m_status->setPagedLoadInfo(firstRow, lastRow, m_pagerState.total, m_colCount,
                                       m_fileBytes, m_openElapsedMs, formatName, libraryCredit);

            // Publishing the state is enough: setPagerState itself applies
            // PageBar::shouldBeVisible(), which hides a single-page file.
            m_pageBar->setPagerState(m_pagerState);

            emit sigCommand(VCT_StateChange, VCV_Loaded);

            // Start async COUNT only once per SQLite table load
            if(!m_isCsv && !m_countRequested && !m_countFailed && !m_pagerState.total.has_value() &&
               m_countWorker) {
                m_countRequested = true;
                QMetaObject::invokeMethod(m_countWorker, "count", Qt::QueuedConnection,
                                          Q_ARG(uint64_t, viewGen), Q_ARG(QString, path),
                                          Q_ARG(QString, tableName));
            }
        });

    connect(m_sourceWorker, &dtv::workers::SourceWorker::refetchRowsCompleted, this,
            [this](uint64_t viewGen, uint64_t copyRequestId, const auto &results) {
                if(!m_isPaged)
                    return;
                if(viewGen != static_cast<uint64_t>(m_generation)) {
                    // The pending copy belongs to the retired view, so it still
                    // has to be completed here or the renderer waits forever.
                    m_renderer->onRefetchRowsCompleted(copyRequestId, {});
                    return;
                }
                m_renderer->onRefetchRowsCompleted(copyRequestId, results);
            });

    connect(m_sourceWorker, &dtv::workers::SourceWorker::sortCompleted, this,
            [this](uint64_t viewGen, uint64_t opGen, bool ok, const QString &error, qint64 total) {
                if(viewGen != static_cast<uint64_t>(m_generation) || !m_isPaged ||
                   opGen != m_opGen->load())
                    return;
                if(m_recoveringSort) {
                    if(ok)
                        finishSortRecovery();
                    else {
                        failSortRecovery(error);
                    }
                    return;
                }
                if(!ok) {
                    m_status->setValueText(tr("Sort failed: %1").arg(error));
                    recoverSort();
                    return;
                }
                m_sortTotalAuthoritative = true;
                m_pagerState.total = total;
                m_pendingPage = 1;
                m_pendingArrivedFromPrev = false;
                m_pageFetchInFlight = true;
                QMetaObject::invokeMethod(m_sourceWorker, "first", Qt::QueuedConnection,
                                          Q_ARG(uint64_t, viewGen), Q_ARG(uint64_t, opGen),
                                          Q_ARG(int, m_pagerState.pageSize));
            });

    if(!m_isCsv && m_countWorker) {
        connect(m_countWorker, &dtv::workers::CountWorker::countCompleted, this,
                [this](uint64_t viewGen, bool ok, const QString &error, qint64 total) {
                    if(viewGen != static_cast<uint64_t>(m_generation) || !m_isPaged)
                        return;

                    ++m_countCompletedCount;

                    if(ok && !m_sortTotalAuthoritative) {
                        m_pagerState.total = total;
                        m_pageBar->setPagerState(m_pagerState);
                        m_status->updatePagedTotal(total);
                    } else if(!ok) {
                        m_countFailed = true;
                        m_status->setWarning(tr("Row count failed: %1").arg(error));
                    }
                });
        m_countThread->start();
    }

    m_sourceThread->start();

    openCurrentSource(m_generation, 1ULL);
}

void DataTableViewer::openCurrentSource(uint64_t viewGen, uint64_t opGen)
{
    if(!m_sourceWorker)
        return;

    if(!m_isCsv) {
        QMetaObject::invokeMethod(m_sourceWorker, "open", Qt::QueuedConnection,
                                  Q_ARG(uint64_t, viewGen), Q_ARG(uint64_t, opGen),
                                  Q_ARG(QString, m_sourcePath), Q_ARG(QString, m_sourceTable));
        return;
    }

    // A zero delimiter asks the source to detect it from the file head.
    const bool isTsv = QFileInfo(m_sourcePath).suffix().compare("tsv", Qt::CaseInsensitive) == 0;
    dtv::workers::SourceOpenDescriptor desc{dtv::workers::SourceKind::Csv, m_sourcePath, "",
                                            isTsv ? '\t' : '\0'};
    QMetaObject::invokeMethod(m_sourceWorker, "openDescriptor", Qt::QueuedConnection,
                              Q_ARG(uint64_t, viewGen), Q_ARG(uint64_t, opGen),
                              Q_ARG(dtv::workers::SourceOpenDescriptor, desc));
}

void DataTableViewer::onSortClicked(int column)
{
    if(!m_isPaged)
        return;
    auto header = m_renderer->horizontalHeader();
    // QHeaderView flips the indicator before emitting sectionClicked.
    header->setSortIndicatorShown(m_sourceOrderValid && m_committedSort.has_value());
    if(m_committedSort)
        header->setSortIndicator(m_committedSort->column, m_committedSort->ascending
                                                              ? Qt::AscendingOrder
                                                              : Qt::DescendingOrder);
    if(!m_canSort) {
        if(m_isCsv && m_status) {
            m_status->setValueText(tr("Sorting is not supported for paged CSV/TSV files"));
        } else if(!m_isCsv && m_status) {
            m_status->setValueText(tr("Sorting is not supported for tables without a rowid"));
        }
        return;
    }
    if(!m_sourceWorker || m_recoveringSort || (m_pageFetchInFlight && !m_sorting))
        return;
    auto previous = m_pendingSort ? m_pendingSort : m_committedSort;
    m_pendingSort =
        SortState{column, !(previous && previous->column == column && previous->ascending)};
    m_sorting = true;
    if(m_status)
        m_status->setSorting(true);
    if(m_pageBar)
        m_pageBar->setBusy(true);
    m_pageTimer.restart();
    auto opGen = m_opGen->fetch_add(1) + 1;
    auto viewGen = m_viewGen->load();
    auto state = *m_pendingSort;
    auto worker = m_sourceWorker;
    QMetaObject::invokeMethod(
        worker,
        [worker, viewGen, opGen, state] {
            worker->sort(viewGen, opGen, state.column, state.ascending);
        },
        Qt::QueuedConnection);
}

void DataTableViewer::cancelSort()
{
    if(m_sorting && !m_recoveringSort)
        recoverSort();
}

void DataTableViewer::failSortRecovery(const QString &error)
{
    m_sourceOrderValid = false;
    m_canSort = false;
    m_sorting = false;
    m_recoveringSort = false;
    if(m_status) {
        m_status->setSorting(false);
        m_status->setValueText(
            tr("Sort recovery failed: %1. Re-enter the table to retry.").arg(error));
    }
    m_renderer->horizontalHeader()->setSortIndicatorShown(false);
    if(m_pageBar)
        m_pageBar->setBusy(true);
}

void DataTableViewer::recoverSort()
{
    m_status->setSorting(false);
    m_pendingSort.reset();
    m_recoveringSort = true;
    m_pageFetchInFlight = false;
    auto opGen = m_opGen->fetch_add(1) + 1;
    // Reopen before restoring: a superseded request may already have promoted
    // its order table before its completion was discarded by the worker.
    openCurrentSource(m_viewGen->load(), opGen);
}

void DataTableViewer::finishSortRecovery()
{
    m_sorting = false;
    m_recoveringSort = false;
    m_status->setSorting(false);
    m_pageBar->setBusy(false);
    auto header = m_renderer->horizontalHeader();
    header->setSortIndicatorShown(m_committedSort.has_value());
    if(m_committedSort)
        header->setSortIndicator(m_committedSort->column, m_committedSort->ascending
                                                              ? Qt::AscendingOrder
                                                              : Qt::DescendingOrder);
}

void DataTableViewer::navigatePage(int64_t targetPage, bool arrivedFromPrev,
                                   std::function<void(uint64_t viewGen, uint64_t opGen)> fetchFunc)
{
    if(!m_isPaged || !m_sourceOrderValid || m_sorting || m_pageFetchInFlight || !m_sourceWorker)
        return;

    m_pageFetchInFlight = true;
    if(m_pageBar)
        m_pageBar->setBusy(true);
    m_pendingPage = targetPage;
    m_pendingArrivedFromPrev = arrivedFromPrev;

    m_pageTimer.restart();

    uint64_t opGen = m_opGen->fetch_add(1) + 1;
    fetchFunc(m_viewGen->load(), opGen);
}

void DataTableViewer::onFirstPageClicked()
{
    if(!m_pagerState.canFirst())
        return;
    navigatePage(1, false, [this](uint64_t vGen, uint64_t oGen) {
        QMetaObject::invokeMethod(m_sourceWorker, "first", Qt::QueuedConnection,
                                  Q_ARG(uint64_t, vGen), Q_ARG(uint64_t, oGen),
                                  Q_ARG(int, m_pagerState.pageSize));
    });
}

void DataTableViewer::onPrevPageClicked()
{
    if(!m_pagerState.canPrev())
        return;
    navigatePage(m_pagerState.page - 1, true, [this](uint64_t vGen, uint64_t oGen) {
        QMetaObject::invokeMethod(m_sourceWorker, "prev", Qt::QueuedConnection,
                                  Q_ARG(uint64_t, vGen), Q_ARG(uint64_t, oGen),
                                  Q_ARG(dtv::core::PageToken, m_currentToken),
                                  Q_ARG(int, m_pagerState.pageSize));
    });
}

void DataTableViewer::onNextPageClicked()
{
    if(!m_pagerState.canNext())
        return;
    navigatePage(m_pagerState.page + 1, false, [this](uint64_t vGen, uint64_t oGen) {
        QMetaObject::invokeMethod(m_sourceWorker, "next", Qt::QueuedConnection,
                                  Q_ARG(uint64_t, vGen), Q_ARG(uint64_t, oGen),
                                  Q_ARG(dtv::core::PageToken, m_currentToken),
                                  Q_ARG(int, m_pagerState.pageSize));
    });
}

void DataTableViewer::onLastPageClicked()
{
    if(!m_pagerState.canLast() || !m_pagerState.total.has_value())
        return;
    navigatePage(m_pagerState.pages(), false, [this](uint64_t vGen, uint64_t oGen) {
        QMetaObject::invokeMethod(m_sourceWorker, "last", Qt::QueuedConnection,
                                  Q_ARG(uint64_t, vGen), Q_ARG(uint64_t, oGen),
                                  Q_ARG(int, m_pagerState.pageSize),
                                  Q_ARG(qint64, *m_pagerState.total));
    });
}

void DataTableViewer::onParseCompleted(std::shared_ptr<const dtv::core::TableParseResult> result,
                                       const QString &tableName, int generation)
{
    m_search->setEnabled(result->ok);

    if(!result->ok) {
        qprintt << "Parse failed:" << QString::fromStdString(result->error);
        m_status->hideLoading();
        m_status->setValueText("Error: " + QString::fromStdString(result->error));
        emit sigCommand(VCT_StateChange, VCV_Error);
        return;
    }

    if(!result->table_names.empty()) {
        qprintt << "SQL table picker ready, count:" << result->table_names.size();
        QStringList tables;
        for(const auto &name : result->table_names) {
            tables << QString::fromStdString(name);
        }
        m_picker->setTables(tables);
        m_stack->setCurrentWidget(m_picker);
        m_backBtn->hide();
        m_search->hide();
        m_pageBar->hide();
        m_status->restoreInfo();
        emit sigCommand(VCT_StateChange, VCV_Loaded);
        return;
    }

    if(result->data) {
        qprintt << "Table data loaded. Rows:" << result->data->rows.size()
                << "Cols:" << result->data->columns.size() << "ms:" << result->elapsed_ms;
        QString format = QString::fromStdString(result->format_name);
        m_renderer->setStateKey(makeKey(format, tableName));

        m_stack->setCurrentWidget(m_renderer);
        m_search->show();
        m_pageBar->hide();
        m_renderer->setData(result->data);
        m_renderer->restoreHeaderState(ini());
        m_status->setLoadInfo(static_cast<int>(result->data->rows.size()),
                              static_cast<int>(result->data->columns.size()), result->file_bytes,
                              result->elapsed_ms, format,
                              QString::fromStdString(result->library_credit),
                              result->data->truncated, result->data->total_rows);

        if(!tableName.isEmpty()) {
            m_backBtn->show();
        } else {
            m_backBtn->hide();
        }

        if(!result->warning.empty()) {
            qprintt << "Warning:" << QString::fromStdString(result->warning);
            m_status->setWarning(QString::fromStdString(result->warning));
        }

        emit sigCommand(VCT_StateChange, VCV_Loaded);
    }
}

QString DataTableViewer::makeKey(const QString &format, const QString &table) const
{
    if(table.isEmpty())
        return format;
    return format + "/" + table;
}

void DataTableViewer::onCopyTriggered()
{
    // A focused input owns clipboard commands; never fall through to the table
    // copy or the table selection would silently overwrite the clipboard.
    if(auto *edit = qobject_cast<QLineEdit *>(focusWidget())) {
        if(edit->hasSelectedText()) {
            edit->copy();
        }
        return;
    }
    if(m_renderer && m_stack->currentWidget() == m_renderer) {
        m_renderer->copyToClipboard();
    }
}

void DataTableViewer::onTextViewBtnClicked()
{
    // The host resolves the path and performs the swap; the plugin only emits
    // the viewer name. The guard keeps an empty-path viewer from emitting.
    if(m_sourcePath.isEmpty() && m_currentPath.isEmpty())
        return;
    emit sigCommand(VCT_LoadViewerWithNewType, QString("Text"));
}
