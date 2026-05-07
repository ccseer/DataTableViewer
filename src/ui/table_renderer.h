#pragma once

#include <QWidget>
#include <memory>
#include "core/table_data.h"

class QTableView;
class QSettings;

namespace dtv {
namespace ui {

class TableModel;
class TableFilterProxy;

class TableRenderer : public QWidget {
    Q_OBJECT
public:
    explicit TableRenderer(QWidget *parent = nullptr);

    void setData(std::shared_ptr<const core::TableData> data);
    void clear();

    void setStateKey(const QString &key); // includes parser-format and table name
    void saveHeaderState(QSettings &settings) const;
    void restoreHeaderState(QSettings &settings);

    void setFilter(const QString &text, int columnScope);
    int filterMatchCount() const;

signals:
    void filterCountChanged(int matches);
    void requestFilter(const QString &text);

public slots:
    void copyToClipboard();
    void copyAsMarkdown();
    void resizeColumnsToFit();
    void filterBySelection();

private slots:
    void showContextMenu(const QPoint &pos);
    void onHeaderClicked(int column);

private:
    void setupView();

    QTableView *m_view = nullptr;
    TableModel *m_model = nullptr;
    TableFilterProxy *m_proxy = nullptr;
    QString m_stateKey;

    int m_lastSortCol = -1;
    Qt::SortOrder m_lastSortOrder = Qt::AscendingOrder;
    bool m_lastSortShown = false;
};

} // namespace ui
} // namespace dtv
