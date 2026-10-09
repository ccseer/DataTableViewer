#pragma once

#include <QWidget>
#include "core/pager_state.h"

class QPushButton;
class QLabel;

namespace dtv::ui {

class PageBar : public QWidget {
    Q_OBJECT
public:
    explicit PageBar(QWidget *parent = nullptr);

    void setPagerState(const core::PagerState &state);
    const core::PagerState &pagerState() const;
    bool shouldBeVisible() const;

    void setBusy(bool busy);
    void updateTheme(bool dark, qreal dpr);
    void setShortcutHints(const QString &first, const QString &prev, const QString &next,
                          const QString &last);

signals:
    void firstClicked();
    void prevClicked();
    void nextClicked();
    void lastClicked();

private:
    void updateDisplay();
    void updateIcons();
    void updateTooltips();

    QPushButton *m_btnFirst = nullptr;
    QPushButton *m_btnPrev = nullptr;
    QLabel *m_labelPage = nullptr;
    QPushButton *m_btnNext = nullptr;
    QPushButton *m_btnLast = nullptr;

    QString m_hintFirst;
    QString m_hintPrev;
    QString m_hintNext;
    QString m_hintLast;

    core::PagerState m_state;
    bool m_busy = false;
    bool m_isDarkMode = false;
    qreal m_dpr = 1.0;
};

} // namespace dtv::ui
