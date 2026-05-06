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

private:
    void setupView();
    void copyToClipboard();

    QTableView *m_view = nullptr;
    TableModel *m_model = nullptr;
    TableFilterProxy *m_proxy = nullptr;
    QString m_stateKey;
};

} // namespace ui
} // namespace dtv
