#include "table_filter_proxy.h"
#include "table_model.h"
#include <cmath>

namespace dtv {
namespace ui {

TableFilterProxy::TableFilterProxy(QObject *parent) : QSortFilterProxyModel(parent)
{
    setSortRole(TableModel::kSortRole);
    setFilterRole(Qt::DisplayRole);
    setFilterCaseSensitivity(Qt::CaseInsensitive);
    setDynamicSortFilter(true);
}

void TableFilterProxy::setFilterText(const QString &text)
{
    m_filterText = text;
    setFilterFixedString(text);
}

void TableFilterProxy::setColumnScope(int column)
{
    m_columnScope = column;
    invalidateFilter();
}

int TableFilterProxy::filterMatchCount() const
{
    return rowCount();
}

bool TableFilterProxy::lessThan(const QModelIndex &source_left,
                                const QModelIndex &source_right) const
{
    QVariant v1 = sourceModel()->data(source_left, TableModel::kSortRole);
    QVariant v2 = sourceModel()->data(source_right, TableModel::kSortRole);

    if(v1.typeId() == QMetaType::Double && v2.typeId() == QMetaType::Double) {
        double d1 = v1.toDouble();
        double d2 = v2.toDouble();
        bool nan1 = std::isnan(d1);
        bool nan2 = std::isnan(d2);

        if(nan1 && nan2)
            return false;
        if(nan1)
            return sortOrder() == Qt::AscendingOrder ? false : true; // NaN last
        if(nan2)
            return sortOrder() == Qt::AscendingOrder ? true : false;

        return d1 < d2;
    }

    return QString::localeAwareCompare(v1.toString(), v2.toString()) < 0;
}

bool TableFilterProxy::filterAcceptsRow(int source_row, const QModelIndex &source_parent) const
{
    if(m_filterText.isEmpty())
        return true;

    if(m_columnScope >= 0) {
        QModelIndex idx = sourceModel()->index(source_row, m_columnScope, source_parent);
        return sourceModel()->data(idx).toString().contains(m_filterText, Qt::CaseInsensitive);
    }

    // All columns
    int cols = sourceModel()->columnCount(source_parent);
    for(int c = 0; c < cols; ++c) {
        QModelIndex idx = sourceModel()->index(source_row, c, source_parent);
        if(sourceModel()->data(idx).toString().contains(m_filterText, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

} // namespace ui
} // namespace dtv
