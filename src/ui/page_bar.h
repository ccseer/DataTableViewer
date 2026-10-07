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

signals:
    void firstClicked();
    void prevClicked();
    void nextClicked();
    void lastClicked();

private:
    void updateDisplay();
    void updateIcons();

    QPushButton *m_btnFirst = nullptr;
    QPushButton *m_btnPrev = nullptr;
    QLabel *m_labelPage = nullptr;
    QPushButton *m_btnNext = nullptr;
    QPushButton *m_btnLast = nullptr;

    core::PagerState m_state;
    bool m_busy = false;
    bool m_isDarkMode = false;
    qreal m_dpr = 1.0;
};

} // namespace dtv::ui
