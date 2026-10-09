#include "table_model.h"
#include "cell_text.h"
#include <cmath>
#include <algorithm>

namespace dtv {
namespace ui {

namespace {
constexpr int kSoftRowLimit = 10000;
constexpr int kFetchBatch = 500;
} // namespace

TableModel::TableModel(QObject *parent) : QAbstractTableModel(parent)
{}

QString TableModel::singleLineDisplayText(const std::string &cell)
{
    return dtv::ui::singleLineDisplayText(cell);
}

void TableModel::setTableData(std::shared_ptr<const core::TableData> data, bool loadAll,
                              int64_t rowOffset)
{
    beginResetModel();
    m_data = data;
    m_rowOffset = rowOffset;
    m_loadedRows = 0;
    if(m_data && loadAll) {
        m_loadedRows = static_cast<int>(m_data->rows.size());
    }
    endResetModel();

    if(m_data && !loadAll && canFetchMore({})) {
        fetchMore({});
    }
}

void TableModel::setRowOffset(int64_t offset)
{
    if(m_rowOffset != offset) {
        m_rowOffset = offset;
        if(m_loadedRows > 0) {
            emit headerDataChanged(Qt::Vertical, 0, m_loadedRows - 1);
        }
    }
}

int TableModel::totalRowCount() const
{
    return m_data ? static_cast<int>(m_data->rows.size()) : 0;
}

int TableModel::rowCount(const QModelIndex &parent) const
{
    if(parent.isValid() || !m_data)
        return 0;
    return m_loadedRows;
}

int TableModel::columnCount(const QModelIndex &parent) const
{
    if(parent.isValid() || !m_data)
        return 0;
    return static_cast<int>(m_data->columns.size());
}

QVariant TableModel::data(const QModelIndex &index, int role) const
{
    if(!index.isValid() || !m_data)
        return {};

    int row = index.row();
    int col = index.column();
    if(row < 0 || row >= static_cast<int>(m_data->rows.size()) || col < 0 ||
       col >= static_cast<int>(m_data->columns.size()))
        return {};

    const std::string &cell = m_data->rows[row][col];

    if(role == Qt::DisplayRole) {
        return singleLineDisplayText(cell);
    }

    if(role == Qt::ToolTipRole) {
        return QString::fromStdString(cell);
    }

    if(role == Qt::TextAlignmentRole) {
        const auto &type = m_data->columns[col].type;
        if(type == core::ColumnMeta::Type::Integer || type == core::ColumnMeta::Type::Float) {
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        }
        return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
    }

    if(role == kSortRole) {
        const auto &type = m_data->columns[col].type;
        if(type == core::ColumnMeta::Type::Integer || type == core::ColumnMeta::Type::Float) {
            auto it = m_data->numeric_cache.by_column.find(col);
            if(it != m_data->numeric_cache.by_column.end()) {
                return it->second[row];
            }
        }
        return QString::fromStdString(cell);
    }

    return {};
}

QVariant TableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if(!m_data)
        return {};

    if(orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        if(section >= 0 && section < static_cast<int>(m_data->columns.size())) {
            return QString::fromStdString(m_data->columns[section].name);
        }
    } else if(orientation == Qt::Vertical && role == Qt::DisplayRole) {
        if(section >= 0 && section < static_cast<int>(m_data->rows.size())) {
            return QString::number(m_rowOffset + section + 1);
        }
    }
    return {};
}

bool TableModel::canFetchMore(const QModelIndex &parent) const
{
    if(parent.isValid() || !m_data)
        return false;
    return m_loadedRows < static_cast<int>(m_data->rows.size()) && m_loadedRows < kSoftRowLimit;
}

void TableModel::fetchMore(const QModelIndex &parent)
{
    if(parent.isValid() || !m_data)
        return;

    int totalAvailable = static_cast<int>(m_data->rows.size());
    int limit = std::min(totalAvailable, kSoftRowLimit);
    int remaining = std::min(kFetchBatch, limit - m_loadedRows);

    if(remaining <= 0)
        return;

    beginInsertRows({}, m_loadedRows, m_loadedRows + remaining - 1);
    m_loadedRows += remaining;
    endInsertRows();
}

} // namespace ui
} // namespace dtv
