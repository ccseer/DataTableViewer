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
#include <QMenu>
#include <QAction>
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

    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QTableView::customContextMenuRequested, this, &TableRenderer::showContextMenu);
    connect(m_view->horizontalHeader(), &QHeaderView::sectionClicked, this,
            &TableRenderer::onHeaderClicked);

    connect(m_proxy, &QAbstractItemModel::modelReset, this, [this] {
        emit filterCountChanged(m_proxy->filterMatchCount());
    });
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, [this] {
        emit filterCountChanged(m_proxy->filterMatchCount());
    });
}

void TableRenderer::setupView()
{
    m_view->setSortingEnabled(false); // Handle 3-state sort manually
    m_view->horizontalHeader()->setSectionsClickable(true);
    m_view->horizontalHeader()->setSortIndicatorShown(false);
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
    m_lastSortShown = false;
    m_lastSortCol = -1;
    m_view->horizontalHeader()->setSortIndicatorShown(false);

    m_proxy->sort(-1); // Reset to original order
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
    // Sort by row, then by column
    std::sort(indices.begin(), indices.end(), [](const QModelIndex &a, const QModelIndex &b) {
        if(a.row() != b.row())
            return a.row() < b.row();
        return a.column() < b.column();
    });

    QString text;
    int lastRow = -1;
    int firstColInRow = -1;

    for(const auto &idx : indices) {
        if(lastRow != -1) {
            if(idx.row() != lastRow) {
                text += "\n";
                firstColInRow = -1;
            } else {
                text += "\t";
            }
        }

        // Get raw data (std::string) and convert to QString
        // Using DisplayRole is fine as TableModel returns the raw string for it.
        text += idx.data(Qt::DisplayRole).toString();

        lastRow = idx.row();
    }

    QGuiApplication::clipboard()->setText(text);
}

void TableRenderer::showContextMenu(const QPoint &pos)
{
    QMenu menu(this);

    QAction *filterAction = menu.addAction(tr("Filter by Selection"));
    filterAction->setEnabled(m_view->selectionModel()->hasSelection());
    connect(filterAction, &QAction::triggered, this, &TableRenderer::filterBySelection);

    menu.addSeparator();

    QAction *copyAction = menu.addAction(tr("Copy"));
    copyAction->setShortcut(QKeySequence::Copy);
    copyAction->setEnabled(m_view->selectionModel()->hasSelection());
    connect(copyAction, &QAction::triggered, this, &TableRenderer::copyToClipboard);

    QAction *copyMdAction = menu.addAction(tr("Copy as Markdown"));
    copyMdAction->setEnabled(m_view->selectionModel()->hasSelection());
    connect(copyMdAction, &QAction::triggered, this, &TableRenderer::copyAsMarkdown);

    menu.addSeparator();

    QAction *resizeAction = menu.addAction(tr("Resize Columns to Fit"));
    connect(resizeAction, &QAction::triggered, this, &TableRenderer::resizeColumnsToFit);

    menu.exec(m_view->viewport()->mapToGlobal(pos));
}

void TableRenderer::onHeaderClicked(int column)
{
    auto *header = m_view->horizontalHeader();

    if(column != m_lastSortCol || !m_lastSortShown) {
        // New column or starting from scratch: Ascending
        m_lastSortCol = column;
        m_lastSortOrder = Qt::AscendingOrder;
        m_lastSortShown = true;
    } else if(m_lastSortOrder == Qt::AscendingOrder) {
        // Move to Descending
        m_lastSortOrder = Qt::DescendingOrder;
    } else {
        // Move to None
        m_proxy->sort(-1);
        header->setSortIndicatorShown(false);
        m_lastSortShown = false;
    }

    header->setSortIndicatorShown(m_lastSortShown);
    if(m_lastSortShown) {
        header->setSortIndicator(m_lastSortCol, m_lastSortOrder);
        m_proxy->sort(m_lastSortCol, m_lastSortOrder);
    } else {
        m_proxy->sort(-1);
    }
}

} // namespace ui
} // namespace dtv
