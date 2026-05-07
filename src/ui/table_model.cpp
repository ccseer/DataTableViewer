#include "table_model.h"
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
    bool hasLineBreak = false;
    for(char ch : cell) {
        if(ch == '\r' || ch == '\n') {
            hasLineBreak = true;
            break;
        }
    }

    if(!hasLineBreak) {
        return QString::fromStdString(cell);
    }

    const QString original = QString::fromStdString(cell);
    QString text;
    text.reserve(original.size());
    bool previousWasNewline = false;

    for(QChar ch : original) {
        if(ch == QLatin1Char('\r') || ch == QLatin1Char('\n')) {
            if(!previousWasNewline) {
                text += QLatin1Char(' ');
                previousWasNewline = true;
            }
            continue;
        }
        text += ch;
        previousWasNewline = false;
    }

    return text;
}

void TableModel::setTableData(std::shared_ptr<const core::TableData> data)
{
    beginResetModel();
    m_data = data;
    m_loadedRows = 0;
    endResetModel();

    if(m_data && canFetchMore({})) {
        fetchMore({});
    }
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
    if(orientation == Qt::Horizontal && role == Qt::DisplayRole && m_data) {
        if(section >= 0 && section < static_cast<int>(m_data->columns.size())) {
            return QString::fromStdString(m_data->columns[section].name);
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
