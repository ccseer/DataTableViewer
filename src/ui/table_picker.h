#pragma once

#include <QWidget>
#include <QStringList>

class QListWidget;
class QLineEdit;
class QLabel;
class QVBoxLayout;

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

    QLabel *m_title = nullptr;
    QLineEdit *m_filter = nullptr;
    QListWidget *m_list = nullptr;
    QVBoxLayout *m_layout = nullptr;
    QStringList m_allTables;

    bool m_isDarkMode = false;
    qreal m_dpr = 1.0;
};

} // namespace ui
} // namespace dtv
