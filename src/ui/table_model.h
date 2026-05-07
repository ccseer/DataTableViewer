#pragma once

#include <QAbstractTableModel>
#include <QString>
#include <memory>
#include "core/table_data.h"

namespace dtv {
namespace ui {

class TableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    static constexpr int kSortRole = Qt::UserRole + 1;

    explicit TableModel(QObject *parent = nullptr);

    void setTableData(std::shared_ptr<const core::TableData> data);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;

private:
    static QString singleLineDisplayText(const std::string &cell);

    std::shared_ptr<const core::TableData> m_data;
    int m_loadedRows = 0;
};

} // namespace ui
} // namespace dtv
