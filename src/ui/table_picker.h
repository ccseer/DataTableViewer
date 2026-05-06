#pragma once

#include <QWidget>
#include <QStringList>

class QListWidget;
class QLineEdit;
class QLabel;

namespace dtv {
namespace ui {

class TablePicker : public QWidget {
    Q_OBJECT
public:
    explicit TablePicker(QWidget *parent = nullptr);

    void setTables(const QStringList &tables);
    void updateTheme(bool dark, qreal dpr);
    void clear();

signals:
    void tableSelected(const QString &name);

private:
    void setupUi();

    QListWidget *m_list = nullptr;
    QLineEdit *m_filter = nullptr;
    QLabel *m_title = nullptr;

    QStringList m_allTables;
};

} // namespace ui
} // namespace dtv
