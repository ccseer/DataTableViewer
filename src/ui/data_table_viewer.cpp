#include "data_table_viewer.h"
#include "search_bar.h"
#include "status_bar.h"
#include "table_renderer.h"
#include "table_picker.h"
#include "style_assets.h"
#include "core/parser_registry.h"
#include "workers/table_worker.h"
#include "workers/background_thread.h"
#include "seer/viewerhelper.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedLayout>
#include <QPushButton>
#include <QShortcut>
#include <QFileInfo>
#include <QPointer>
#include <QSettings>
#include <QDebug>
#include <QThread>
#include <QCoreApplication>
#include <QSqlDatabase>

#define qprintt qDebug() << "[DataTableViewer]"

namespace {
bool isSqliteExtension(const QString &suffix)
{
    const QString ext = suffix.toLower();
    return ext == "sqlite" || ext == "sqlite3" || ext == "db" || ext == "db3" || ext == "sl3";
}
} // namespace

DataTableViewer::DataTableViewer(QWidget *parent) : ViewerBase(parent)
{
    qprintt << this;
}

DataTableViewer::~DataTableViewer()
{
    cancelPending();

    qprintt << "~" << this;
}

void DataTableViewer::init()
{
    qRegisterMetaType<std::shared_ptr<const dtv::core::TableParseResult>>(
        "std::shared_ptr<const dtv::core::TableParseResult>");
    dtv::core::ParserRegistry::instance().registerBuiltinParsers();

    // 1. Create UI components first
    m_search = new dtv::ui::SearchBar(this);
    m_status = new dtv::ui::StatusBar(this);
    m_renderer = new dtv::ui::TableRenderer(this);
    m_picker = new dtv::ui::TablePicker(this);

    // 2. Setup paths and check drivers
    const QString path = seer::getDLLPath();
    if(!path.isEmpty()) {
        QCoreApplication::addLibraryPath(path);
    } else {
        qprintt << "get DLL path failed" << path;
    }

    m_sqliteAvailable = QSqlDatabase::drivers().contains("QSQLITE");
    if(!m_sqliteAvailable) {
        qprintt << "No SQL drivers found. SQLite support will report an error if used.";
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
        m_stack->setCurrentWidget(m_picker);
        m_backBtn->hide();
        m_search->hide();
        m_status->restoreInfo();
    });

    connect(m_picker, &dtv::ui::TablePicker::tableSelected, this, [this](const QString &name) {
        doLoadFile(m_currentPath, name);
    });

    connect(m_search, &dtv::ui::SearchBar::filterChanged, this, [this](const QString &text) {
        m_renderer->setFilter(text, -1);
    });

    connect(m_renderer, &dtv::ui::TableRenderer::requestFilter, this, [this](const QString &text) {
        m_search->setText(text);
    });

    connect(m_renderer, &dtv::ui::TableRenderer::currentItemChanged, this,
            [this](const QString &header, const QString &value) {
                if(header.isEmpty()) {
                    m_status->restoreInfo();
                } else {
                    m_status->setValueText(QString("%1 : %2").arg(header).arg(value));
                }
            });

    connect(m_renderer, &dtv::ui::TableRenderer::filterCountChanged, this, [this](int matches) {
        m_status->setFilterMatchCount(matches, !m_search->text().isEmpty());
    });

    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    findShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(findShortcut, &QShortcut::activated, this, [this] {
        m_search->setFocus();
        m_search->selectAll();
    });
}

void DataTableViewer::loadImpl(QBoxLayout *lay_content, QHBoxLayout *lay_ctrlbar)
{
    init();

    lay_content->setContentsMargins(0, 0, 0, 0);
    lay_content->setSpacing(qRound(6 * m_dpr));
    lay_content->addWidget(m_search);
    lay_content->addLayout(m_stack, 1);
    lay_content->addWidget(m_status);

    if(auto *slay = qobject_cast<QHBoxLayout *>(m_search->layout())) {
        slay->insertWidget(0, m_backBtn);
    }

    m_currentPath = options()->path();
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
    if(m_picker)
        m_picker->updateTheme(m_isDarkMode, r);

    if(layout()) {
        layout()->setSpacing(qRound(6 * r));
    }

    reapplyStyles();
}

void DataTableViewer::updateTheme(int theme)
{
    m_isDarkMode = (theme == 1);
    m_search->updateTheme(m_isDarkMode);
    m_status->updateTheme(m_isDarkMode, m_dpr);
    m_picker->updateTheme(m_isDarkMode, m_dpr);
    reapplyStyles();
}

void DataTableViewer::reapplyStyles()
{
    using namespace dtv::ui;
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
}

void DataTableViewer::cancelPending()
{
    emit cancelRequested();
    m_generation++;
}

void DataTableViewer::doLoadFile(const QString &path, const QString &tableName)
{
    cancelPending();

    m_renderer->clear();
    m_search->clear();
    m_status->showLoading();
    m_search->setEnabled(false);

    QFileInfo info(path);
    qprintt << "doLoadFile:" << path << "table:" << tableName << "suffix:" << info.suffix();

    if(isSqliteExtension(info.suffix()) && !m_sqliteAvailable) {
        m_status->setValueText("Error: SQLite driver not loaded.");
        emit sigCommand(VCT_StateChange, VCV_Error);
        return;
    }

    auto parser = dtv::core::ParserRegistry::instance().createParser(info.suffix().toStdString());
    if(!parser) {
        qprintt << "Error: No parser found for extension:" << info.suffix();
        m_status->setValueText("No parser found for extension: " + info.suffix());
        emit sigCommand(VCT_StateChange, VCV_Error);
        return;
    }

    auto *thread = new BackgroundThread;
    auto *worker = new dtv::workers::TableWorker(std::move(parser), path, tableName, m_generation);
    QPointer<dtv::workers::TableWorker> wp = worker;

    connect(this, &DataTableViewer::destroyed, this, [thread, wp] {
        if(wp)
            thread->requestInterruption();
    });
    connect(this, &DataTableViewer::cancelRequested, this, [thread, wp] {
        if(wp)
            thread->requestInterruption();
    });
    connect(worker, &QObject::destroyed, thread, &QObject::deleteLater);

    connect(thread, &QThread::started, worker, &dtv::workers::TableWorker::doParse);
    connect(worker, &dtv::workers::TableWorker::parseCompleted, this,
            [this](auto result, QString tName, int wgen) {
                if(wgen != m_generation)
                    return;
                onParseCompleted(result, tName, wgen);
            });

    worker->moveToThread(thread);
    thread->start();
}

void DataTableViewer::onParseCompleted(std::shared_ptr<const dtv::core::TableParseResult> result,
                                       const QString &tableName, int generation)
{
    m_search->setEnabled(result->ok);

    if(!result->ok) {
        qprintt << "Parse failed:" << QString::fromStdString(result->error);
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
        m_status->restoreInfo();
        emit sigCommand(VCT_StateChange, VCV_Loaded);
        return;
    }

    if(result->data) {
        qprintt << "Table data loaded. Rows:" << result->data->rows.size()
                << "Cols:" << result->data->columns.size() << "ms:" << result->elapsed_ms;
        QString format = QString::fromStdString(result->format_name);
        m_renderer->setStateKey(makeKey(format, tableName));

        QSettings settings(QSettings::IniFormat, QSettings::UserScope, "ccseer", "Seer");
        m_stack->setCurrentWidget(m_renderer);
        m_search->show();
        m_renderer->setData(result->data);
        m_renderer->restoreHeaderState(settings);
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
    if(m_renderer && m_stack->currentWidget() == m_renderer) {
        m_renderer->copyToClipboard();
    }
}
