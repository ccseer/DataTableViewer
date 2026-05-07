#include "data_table_viewer.h"
#include "search_bar.h"
#include "status_bar.h"
#include "table_renderer.h"
#include "table_picker.h"
#include "style_assets.h"
#include "core/parser_registry.h"
#include "workers/table_worker.h"
#include "workers/background_thread.h"

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

#define qprintt qDebug() << "[DataTableViewer]"

DataTableViewer::DataTableViewer(QWidget *parent) : ViewerBase(parent)
{}

DataTableViewer::~DataTableViewer()
{
    cancelPending();
}

void DataTableViewer::init()
{
    if(m_search)
        return;

    qRegisterMetaType<std::shared_ptr<const dtv::core::TableParseResult>>(
        "std::shared_ptr<const dtv::core::TableParseResult>");
    dtv::core::ParserRegistry::instance().registerBuiltinParsers();

    m_search = new dtv::ui::SearchBar(this);
    m_status = new dtv::ui::StatusBar(this);
    m_renderer = new dtv::ui::TableRenderer(this);
    m_picker = new dtv::ui::TablePicker(this);

    m_stack = new QStackedLayout;
    m_stack->addWidget(m_renderer);
    m_stack->addWidget(m_picker);

    m_backBtn = new QPushButton("← Tables", this);
    m_backBtn->hide();
    m_backBtn->setCursor(Qt::PointingHandCursor);
    m_backBtn->setStyleSheet("QPushButton { border: none; background: transparent; padding: 4px "
                             "8px; border-radius: 4px; font-weight: bold; }"
                             "QPushButton:hover { background-color: rgba(128, 128, 128, 40); }");

    connect(m_backBtn, &QPushButton::clicked, this, [this] {
        m_stack->setCurrentWidget(m_picker);
        m_backBtn->hide();
        m_status->clear();
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
    if(!lay_content)
        return;

    init();

    lay_content->setContentsMargins(0, 0, 0, 0);
    lay_content->setSpacing(qRound(6 * m_dpr));
    if(m_search)
        lay_content->addWidget(m_search);
    if(m_stack)
        lay_content->addLayout(m_stack, 1);
    if(m_status)
        lay_content->addWidget(m_status);

    if(lay_ctrlbar && m_backBtn) {
        lay_ctrlbar->insertWidget(0, m_backBtn);
    }

    if(auto *opt = options()) {
        m_currentPath = opt->path();
        updateTheme(opt->theme());
        updateDPR(opt->dpr());
    }

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
    if(m_search)
        m_search->updateTheme(m_isDarkMode);
    if(m_status)
        m_status->updateTheme(m_isDarkMode, m_dpr);
    if(m_picker)
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

    if(m_search) {
        m_search->setStyleSheet(QString(g_qss_top_bar)
                                    .arg(surface, border, input, text, accent)
                                    .arg(qRound(6 * m_dpr))
                                    .arg(qRound(4 * m_dpr))
                                    .arg(qRound(8 * m_dpr)));
    }
    if(m_status) {
        m_status->setStyleSheet(QString(g_qss_bottom_bar).arg(surface, border));
    }

    if(m_backBtn) {
        QColor textColor(m_isDarkMode ? Colors::DarkText : Colors::LightText);
        m_backBtn->setStyleSheet(
            QString("QPushButton { border: none; background: transparent; padding: %1px %2px; "
                    "border-radius: 4px; font-weight: bold; color: %3; }"
                    "QPushButton:hover { background-color: rgba(128, 128, 128, 40); }")
                .arg(qRound(4 * m_dpr))
                .arg(qRound(8 * m_dpr))
                .arg(textColor.name()));
    }
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
    auto parser = dtv::core::ParserRegistry::instance().createParser(info.suffix().toStdString());
    if(!parser) {
        m_status->setValueText("No parser found for extension: " + info.suffix());
        sigCommand(VCT_StateChange, VCV_Error);
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
        m_status->setValueText("Error: " + QString::fromStdString(result->error));
        sigCommand(VCT_StateChange, VCV_Error);
        return;
    }

    if(!result->table_names.empty()) {
        QStringList tables;
        for(const auto &name : result->table_names) {
            tables << QString::fromStdString(name);
        }
        m_picker->setTables(tables);
        m_stack->setCurrentWidget(m_picker);
        m_backBtn->hide();
        m_search->hide();
        m_status->restoreInfo();
        sigCommand(VCT_StateChange, VCV_Loaded);
        return;
    }

    if(result->data) {
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
            m_status->setWarning(QString::fromStdString(result->warning));
        }

        sigCommand(VCT_StateChange, VCV_Loaded);
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
