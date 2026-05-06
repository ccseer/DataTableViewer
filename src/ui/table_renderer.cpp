#include "table_renderer.h"
#include "table_model.h"
#include "table_filter_proxy.h"

#include <QTableView>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QSettings>
#include <QShortcut>
#include <QKeySequence>
#include <QGuiApplication>
#include <QClipboard>
#include <QDebug>

namespace dtv {
namespace ui {

TableRenderer::TableRenderer(QWidget *parent) : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);

    m_view = new QTableView(this);
    m_model = new TableModel(this);
    m_proxy = new TableFilterProxy(this);
    m_proxy->setSourceModel(m_model);
    m_view->setModel(m_proxy);

    setupView();
    lay->addWidget(m_view);

    auto *copyShortcut = new QShortcut(QKeySequence::Copy, m_view);
    copyShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copyShortcut, &QShortcut::activated, this, &TableRenderer::copyToClipboard);

    connect(m_proxy, &QAbstractItemModel::modelReset, this, [this] {
        emit filterCountChanged(m_proxy->filterMatchCount());
    });
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, [this] {
        emit filterCountChanged(m_proxy->filterMatchCount());
    });
}

void TableRenderer::setupView()
{
    m_view->setSortingEnabled(true);
    m_view->horizontalHeader()->setSortIndicatorShown(true);
    m_view->horizontalHeader()->setStretchLastSection(false);
    m_view->horizontalHeader()->setSectionsMovable(true);
    m_view->horizontalHeader()->setDefaultSectionSize(120);
    m_view->verticalHeader()->setDefaultSectionSize(22);
    m_view->verticalHeader()->hide();
    m_view->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_view->setAlternatingRowColors(true);
    m_view->setWordWrap(false);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setTabKeyNavigation(false);
}

void TableRenderer::setData(std::shared_ptr<const core::TableData> data)
{
    m_model->setTableData(data);
    m_proxy->invalidate();
}

void TableRenderer::clear()
{
    m_model->setTableData(nullptr);
}

void TableRenderer::setStateKey(const QString &key)
{
    m_stateKey = key;
}

void TableRenderer::saveHeaderState(QSettings &settings) const
{
    if(m_stateKey.isEmpty())
        return;

    settings.beginGroup("TablePlugin/" + m_stateKey);
    settings.setValue("HeaderState", m_view->horizontalHeader()->saveState());
    settings.setValue("SortColumn", m_view->horizontalHeader()->sortIndicatorSection());
    settings.setValue("SortOrder", (int)m_view->horizontalHeader()->sortIndicatorOrder());
    settings.endGroup();
}

void TableRenderer::restoreHeaderState(QSettings &settings)
{
    if(m_stateKey.isEmpty())
        return;

    settings.beginGroup("TablePlugin/" + m_stateKey);
    if(settings.contains("HeaderState")) {
        m_view->horizontalHeader()->restoreState(settings.value("HeaderState").toByteArray());
    }
    if(settings.contains("SortColumn")) {
        int col = settings.value("SortColumn").toInt();
        Qt::SortOrder order = (Qt::SortOrder)settings.value("SortOrder").toInt();
        m_view->sortByColumn(col, order);
    }
    settings.endGroup();
}

void TableRenderer::setFilter(const QString &text, int columnScope)
{
    m_proxy->setColumnScope(columnScope);
    m_proxy->setFilterText(text);
    emit filterCountChanged(m_proxy->filterMatchCount());
}

int TableRenderer::filterMatchCount() const
{
    return m_proxy->filterMatchCount();
}

void TableRenderer::copyToClipboard()
{
    auto *selection = m_view->selectionModel();
    if(!selection->hasSelection())
        return;

    auto indices = selection->selectedIndexes();
    std::sort(indices.begin(), indices.end(), [](const QModelIndex &a, const QModelIndex &b) {
        if(a.row() != b.row())
            return a.row() < b.row();
        return a.column() < b.column();
    });

    QString text;
    int lastRow = -1;
    for(const auto &idx : indices) {
        if(lastRow != -1) {
            if(idx.row() != lastRow)
                text += "\n";
            else
                text += "\t";
        }
        text += idx.data(Qt::DisplayRole).toString();
        lastRow = idx.row();
    }

    QGuiApplication::clipboard()->setText(text);
}

} // namespace ui
} // namespace dtv
