#pragma once

#include <QSortFilterProxyModel>

namespace dtv {
namespace ui {

class TableFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit TableFilterProxy(QObject *parent = nullptr);

    void setFilterText(const QString &text);
    void setColumnScope(int column); // -1 = all columns
    int filterMatchCount() const;

protected:
    bool lessThan(const QModelIndex &source_left, const QModelIndex &source_right) const override;
    bool filterAcceptsRow(int source_row, const QModelIndex &source_parent) const override;

private:
    int m_columnScope = -1;
    QString m_filterText;
};

} // namespace ui
} // namespace dtv
