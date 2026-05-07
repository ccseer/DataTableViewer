#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QTimer;

namespace dtv {
namespace ui {

class SearchBar : public QWidget {
    Q_OBJECT
public:
    explicit SearchBar(QWidget *parent = nullptr);

    QString text() const;
    void setText(const QString &text);
    void clear();
    void selectAll();
    void updateDPR(qreal r);
    void updateTheme(bool dark);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

signals:
    void filterChanged(const QString &text);
    void nextMatch();
    void prevMatch();

private:
    void refreshIcon();
    void positionIcon();

    QLabel *m_searchIcon = nullptr;
    QLineEdit *m_edit = nullptr;
    QTimer *m_timer = nullptr;
    qreal m_dpr = 1.0;
    bool m_isDarkMode = false;
};

} // namespace ui
} // namespace dtv
