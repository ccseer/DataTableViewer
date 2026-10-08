#include "table_renderer.h"
#include "cell_text.h"
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
#include <QKeyEvent>
#include <QScrollBar>
#include <QDebug>
#include <set>
#include <unordered_map>

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
    m_view->installEventFilter(this);

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

    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current, const QModelIndex & /*previous*/) {
                if(current.isValid()) {
                    QString header =
                        m_proxy->headerData(current.column(), Qt::Horizontal).toString();
                    QString value = current.data(Qt::DisplayRole).toString();
                    emit currentItemChanged(header, value);
                } else {
                    emit currentItemChanged("", "");
                }
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
    m_view->horizontalHeader()->setResizeContentsPrecision(100);
}

void TableRenderer::setData(std::shared_ptr<const core::TableData> data)
{
    m_pagedMode = false;
    m_pageKeys.clear();
    m_pageClamped.clear();
    m_pendingCopy.reset();
    m_lastSortShown = false;
    m_lastSortCol = -1;
    m_view->horizontalHeader()->setSortIndicatorShown(false);

    m_proxy->sort(-1); // Reset to original order
    m_model->setTableData(data, false);
    m_proxy->invalidate();
}

void TableRenderer::setPageData(std::shared_ptr<const core::TableData> data,
                                std::vector<core::RefetchKey> keys,
                                std::vector<std::vector<bool>> clamped)
{
    m_pagedMode = true;
    m_pageKeys = std::move(keys);
    m_pageClamped = std::move(clamped);
    m_model->setTableData(data, true);
    m_proxy->invalidate();
    if(m_view && m_model->rowCount() > 0) {
        m_view->scrollTo(m_proxy->index(0, 0), QAbstractItemView::PositionAtTop);
    }
}

void TableRenderer::clear()
{
    m_pagedMode = false;
    m_pageKeys.clear();
    m_pageClamped.clear();
    m_pendingCopy.reset();
    m_lastSortShown = false;
    m_lastSortCol = -1;
    m_view->horizontalHeader()->setSortIndicatorShown(false);
    m_proxy->sort(-1);
    m_model->setTableData(nullptr);
}

void TableRenderer::setPagedMode(bool paged)
{
    m_pagedMode = paged;
}

bool TableRenderer::isPagedMode() const
{
    return m_pagedMode;
}

int TableRenderer::rowCount() const
{
    return m_model ? m_model->rowCount() : 0;
}

TableModel *TableRenderer::model() const
{
    return m_model;
}

QHeaderView *TableRenderer::horizontalHeader() const
{
    return m_view ? m_view->horizontalHeader() : nullptr;
}

bool TableRenderer::isCellClamped(int modelRow, int modelCol) const
{
    if(!m_pagedMode)
        return false;
    if(modelRow < 0 || modelRow >= static_cast<int>(m_pageClamped.size()))
        return false;
    if(modelCol < 0 || modelCol >= static_cast<int>(m_pageClamped[modelRow].size()))
        return false;
    return m_pageClamped[modelRow][modelCol];
}

bool TableRenderer::isSelectedCellClamped() const
{
    auto *selection = m_view->selectionModel();
    if(!selection || selection->selectedIndexes().size() != 1)
        return false;
    QModelIndex srcIdx = m_proxy->mapToSource(selection->selectedIndexes().first());
    return isCellClamped(srcIdx.row(), srcIdx.column());
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

void TableRenderer::performCopy(bool isMarkdown)
{
    auto *selection = m_view->selectionModel();
    if(!selection || !selection->hasSelection())
        return;

    auto indices = selection->selectedIndexes();
    auto *header = m_view->horizontalHeader();

    // Sort by visual row, then by visual column
    std::sort(indices.begin(), indices.end(), [header](const QModelIndex &a, const QModelIndex &b) {
        if(a.row() != b.row())
            return a.row() < b.row();
        int visA = header->visualIndex(a.column());
        int visB = header->visualIndex(b.column());
        return visA < visB;
    });

    PendingCopy pending;
    pending.isMarkdown = isMarkdown;
    pending.copyRequestId = ++m_currentCopyRequestId;
    std::vector<std::pair<int, core::RefetchKey>> rowsToRefetch;
    std::map<int, std::vector<int>> rowSelectedCols;

    for(const auto &idx : indices) {
        QModelIndex srcIdx = m_proxy->mapToSource(idx);
        int mRow = srcIdx.row();
        int mCol = srcIdx.column();
        bool clamped = isCellClamped(mRow, mCol);

        PendingCopyCell cell;
        cell.visRow = idx.row();
        cell.visCol = header->visualIndex(idx.column());
        cell.modelRow = mRow;
        cell.modelCol = mCol;
        cell.displayedText = idx.data(Qt::DisplayRole).toString();
        cell.headerText = m_proxy->headerData(idx.column(), Qt::Horizontal).toString();
        cell.clamped = clamped;
        pending.cells.push_back(cell);

        if(clamped && mRow >= 0 && mRow < static_cast<int>(m_pageKeys.size())) {
            auto &cols = rowSelectedCols[mRow];
            if(std::find(cols.begin(), cols.end(), mCol) == cols.end()) {
                cols.push_back(mCol);
            }
        }
    }

    for(auto &pair : rowSelectedCols) {
        std::sort(pair.second.begin(), pair.second.end());
        int mRow = pair.first;
        core::RefetchKey key = m_pageKeys[mRow];
        key.selectedColumns = std::move(pair.second);
        rowsToRefetch.emplace_back(mRow, std::move(key));
    }

    if(rowsToRefetch.empty()) {
        m_pendingCopy.reset();
        // No cells need refetching: write directly to clipboard!
        QGuiApplication::clipboard()->setText(isMarkdown ? buildMarkdownText(pending.cells, {})
                                                         : buildPlainText(pending.cells, {}));
        return;
    }

    // Clamped cells exist: issue asynchronous refetch!
    m_pendingCopy = std::move(pending);
    emit refetchRowsRequested(m_currentCopyRequestId, isMarkdown, rowsToRefetch);
}

QString TableRenderer::rawCellText(const PendingCopyCell &cell,
                                   const std::unordered_map<int, std::unordered_map<int, std::string>> &refetched) const
{
    if(cell.clamped) {
        auto rowIt = refetched.find(cell.modelRow);
        if(rowIt != refetched.end()) {
            auto colIt = rowIt->second.find(cell.modelCol);
            if(colIt != rowIt->second.end()) {
                return singleLineDisplayText(colIt->second);
            }
        }
    }
    return cell.displayedText;
}

QString TableRenderer::buildPlainText(const std::vector<PendingCopyCell> &cells,
                                      const std::unordered_map<int, std::unordered_map<int, std::string>> &refetched) const
{
    QString text;
    int lastRow = -1;
    for(const auto &cell : cells) {
        if(lastRow != -1) {
            text += (cell.visRow != lastRow) ? "\n" : "\t";
        }
        text += rawCellText(cell, refetched);
        lastRow = cell.visRow;
    }
    return text;
}

QString TableRenderer::buildMarkdownText(const std::vector<PendingCopyCell> &cells,
                                         const std::unordered_map<int, std::unordered_map<int, std::string>> &refetched) const
{
    std::set<int> rowSet, colSet;
    for(const auto &c : cells) {
        rowSet.insert(c.visRow);
        colSet.insert(c.visCol);
    }
    std::vector<int> rows(rowSet.begin(), rowSet.end());
    std::vector<int> cols(colSet.begin(), colSet.end());

    // Safety limit for Markdown formatting
    const size_t kMaxMdRows = 1000;
    bool truncated = false;
    if(rows.size() > kMaxMdRows) {
        rows.resize(kMaxMdRows);
        truncated = true;
    }

    QString text = "|";
    for(int col : cols) {
        auto found = std::find_if(cells.begin(), cells.end(), [col](const auto &c) { return c.visCol == col; });
        QString h = singleLineDisplayText(found->headerText.toStdString());
        h.replace("|", "\\|");
        text += h + "|";
    }
    text += "\n|";
    for(size_t i = 0; i < cols.size(); ++i) {
        text += "---|";
    }
    text += "\n";

    // performCopy sorts cells by visual row then visual column, so one advancing
    // cursor fills the whole grid in a single linear pass.
    size_t cell = 0;
    for(int row : rows) {
        text += "|";
        for(int col : cols) {
            while(cell < cells.size() &&
                  (cells[cell].visRow < row ||
                   (cells[cell].visRow == row && cells[cell].visCol < col))) {
                ++cell;
            }

            const bool present = cell < cells.size() && cells[cell].visRow == row &&
                                 cells[cell].visCol == col;
            if(present) {
                QString val = rawCellText(cells[cell], refetched);
                val.replace("|", "\\|");
                text += val + "|";
            } else {
                text += " |";
            }
        }
        text += "\n";
    }
    if(truncated) {
        text += QString("\n\n*(Truncated: Only first %1 selected rows were copied as Markdown)*").arg(kMaxMdRows);
    }
    return text;
}

void TableRenderer::copyToClipboard()
{
    performCopy(false);
}

void TableRenderer::copyAsMarkdown()
{
    performCopy(true);
}

void TableRenderer::onRefetchRowsCompleted(uint64_t copyRequestId,
                                          const std::vector<std::pair<int, core::RefetchResult>> &results)
{
    if(copyRequestId != m_currentCopyRequestId || !m_pendingCopy || m_pendingCopy->copyRequestId != copyRequestId) {
        return;
    }

    bool budgetExceeded = false;
    for(const auto &res : results) {
        if(!res.second.ok && res.second.error == core::kCopyBudgetExceededError) {
            budgetExceeded = true;
            break;
        }
    }

    if(budgetExceeded) {
        m_pendingCopy.reset();
        emit copyRefetchIncomplete(-1);
        return;
    }

    std::unordered_map<int, std::unordered_map<int, std::string>> refetchedValues;
    for(const auto &res : results) {
        if(res.second.ok) {
            auto &rowMap = refetchedValues[res.first];
            if(!res.second.columns.empty()) {
                for(size_t i = 0; i < res.second.columns.size() && i < res.second.values.size(); ++i) {
                    rowMap[res.second.columns[i]] = res.second.values[i];
                }
            } else {
                for(size_t i = 0; i < res.second.values.size(); ++i) {
                    rowMap[static_cast<int>(i)] = res.second.values[i];
                }
            }
        }
    }

    // Rows whose clamped cells could not be refreshed keep the truncated display
    // value; surface that so the user knows the copy may be incomplete.
    std::set<int> failedRows;
    for(const auto &cell : m_pendingCopy->cells) {
        if(cell.clamped) {
            auto it = refetchedValues.find(cell.modelRow);
            if(it == refetchedValues.end() || it->second.find(cell.modelCol) == it->second.end()) {
                failedRows.insert(cell.modelRow);
            }
        }
    }

    const bool isMarkdown = m_pendingCopy->isMarkdown;
    QGuiApplication::clipboard()->setText(isMarkdown ? buildMarkdownText(m_pendingCopy->cells, refetchedValues)
                                                     : buildPlainText(m_pendingCopy->cells, refetchedValues));

    m_pendingCopy.reset();

    if(!failedRows.empty()) {
        emit copyRefetchIncomplete(static_cast<int>(failedRows.size()));
    }
}

void TableRenderer::selectCell(int row, int col)
{
    if(!m_model || !m_proxy || !m_view || !m_view->selectionModel())
        return;
    QModelIndex proxyIdx = m_proxy->mapFromSource(m_model->index(row, col));
    if(!proxyIdx.isValid())
        return;
    m_view->selectionModel()->select(proxyIdx, QItemSelectionModel::ClearAndSelect);
}

void TableRenderer::showContextMenu(const QPoint &pos)
{
    QMenu menu(this);

    QAction *filterAction = menu.addAction(tr("Filter by Selection"));
    filterAction->setEnabled(m_view->selectionModel()->selectedIndexes().size() == 1 &&
                             !isSelectedCellClamped());
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

bool TableRenderer::eventFilter(QObject *obj, QEvent *event)
{
    if(obj == m_view && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if(m_pagedMode) {
            const bool hasCtrl = (keyEvent->modifiers() & Qt::ControlModifier);
            auto *vBar = m_view->verticalScrollBar();
            const bool atTop = (!vBar || vBar->value() <= vBar->minimum());
            const bool atBottom = (!vBar || vBar->value() >= vBar->maximum());

            if(keyEvent->key() == Qt::Key_PageUp) {
                if(hasCtrl || atTop) {
                    emit pageUpRequested();
                    return true;
                }
            } else if(keyEvent->key() == Qt::Key_PageDown) {
                if(hasCtrl || atBottom) {
                    emit pageDownRequested();
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void TableRenderer::onHeaderClicked(int column)
{
    if(m_pagedMode) {
        // The viewer owns server-side sorting; the proxy must keep page order.
        return;
    }

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

void TableRenderer::resizeColumnsToFit()
{
    m_view->resizeColumnsToContents();
}

void TableRenderer::filterBySelection()
{
    auto indices = m_view->selectionModel()->selectedIndexes();
    if(indices.isEmpty() || isSelectedCellClamped())
        return;

    emit requestFilter(indices.first().data(Qt::DisplayRole).toString());
}

} // namespace ui
} // namespace dtv
