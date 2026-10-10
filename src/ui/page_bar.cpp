#include "page_bar.h"
#include "style_assets.h"

#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>

namespace dtv::ui {

PageBar::PageBar(QWidget *parent) : QWidget(parent)
{
    setObjectName("pageBar");

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 0, 12, 0);
    layout->setSpacing(6);

    layout->addStretch();

    m_btnFirst = new QPushButton(this);
    layout->addWidget(m_btnFirst);

    m_btnPrev = new QPushButton(this);
    layout->addWidget(m_btnPrev);

    m_labelPage = new QLabel(this);
    m_labelPage->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_labelPage);

    m_btnNext = new QPushButton(this);
    layout->addWidget(m_btnNext);

    m_btnLast = new QPushButton(this);
    layout->addWidget(m_btnLast);

    layout->addStretch();

    connect(m_btnFirst, &QPushButton::clicked, this, &PageBar::firstClicked);
    connect(m_btnPrev, &QPushButton::clicked, this, &PageBar::prevClicked);
    connect(m_btnNext, &QPushButton::clicked, this, &PageBar::nextClicked);
    connect(m_btnLast, &QPushButton::clicked, this, &PageBar::lastClicked);

    updateTooltips();
    updateTheme(false, 1.0);
    updateDisplay();
}

void PageBar::setPagerState(const core::PagerState &state)
{
    m_state = state;
    updateDisplay();
    setVisible(shouldBeVisible());
}

const core::PagerState &PageBar::pagerState() const
{
    return m_state;
}

bool PageBar::shouldBeVisible() const
{
    if(m_state.page <= 1 && !m_state.hasMore &&
       (!m_state.total.has_value() || *m_state.total <= m_state.pageSize)) {
        return false;
    }
    return true;
}

void PageBar::setBusy(bool busy)
{
    m_busy = busy;
    updateDisplay();
}

void PageBar::updateTheme(bool dark, qreal dpr)
{
    m_isDarkMode = dark;
    m_dpr = dpr;

    setFixedHeight(qRound(28 * m_dpr));

    const int btnSize = qRound(24 * m_dpr);
    m_btnFirst->setFixedSize(btnSize, btnSize);
    m_btnPrev->setFixedSize(btnSize, btnSize);
    m_btnNext->setFixedSize(btnSize, btnSize);
    m_btnLast->setFixedSize(btnSize, btnSize);

    if(auto *lay = qobject_cast<QHBoxLayout *>(layout())) {
        lay->setContentsMargins(qRound(12 * m_dpr), 0, qRound(12 * m_dpr), 0);
        lay->setSpacing(qRound(6 * m_dpr));
    }

    const char *surface = m_isDarkMode ? Colors::DarkSurface : Colors::LightSurface;
    const char *border = m_isDarkMode ? Colors::DarkBorder : Colors::LightBorder;
    const char *text = m_isDarkMode ? Colors::DarkText : Colors::LightText;

    setStyleSheet(QString(g_qss_page_bar)
                      .arg(surface, border)
                      .arg(qRound(4 * m_dpr))
                      .arg(text)
                      .arg(qRound(14 * m_dpr)));

    updateIcons();
}

void PageBar::setShortcutHints(const QString &first, const QString &prev, const QString &next,
                               const QString &last)
{
    m_hintFirst = first;
    m_hintPrev = prev;
    m_hintNext = next;
    m_hintLast = last;
    updateTooltips();
}

void PageBar::updateTooltips()
{
    m_btnFirst->setToolTip(m_hintFirst.isEmpty() ? tr("First page")
                                                 : tr("First page (%1)").arg(m_hintFirst));
    m_btnPrev->setToolTip(m_hintPrev.isEmpty() ? tr("Previous page")
                                               : tr("Previous page (%1)").arg(m_hintPrev));
    m_btnNext->setToolTip(m_hintNext.isEmpty() ? tr("Next page")
                                               : tr("Next page (%1)").arg(m_hintNext));
    m_btnLast->setToolTip(m_hintLast.isEmpty() ? tr("Last page")
                                               : tr("Last page (%1)").arg(m_hintLast));
}

void PageBar::updateIcons()
{
    QColor normalColor(m_isDarkMode ? Colors::DarkText : Colors::LightText);
    const int iconSize = qRound(16 * m_dpr);

    m_btnFirst->setIcon(createIcon(g_svg_first_page, normalColor, iconSize));
    m_btnFirst->setIconSize(QSize(iconSize, iconSize));

    m_btnPrev->setIcon(createIcon(g_svg_chevron_left, normalColor, iconSize));
    m_btnPrev->setIconSize(QSize(iconSize, iconSize));

    m_btnNext->setIcon(createIcon(g_svg_chevron_right, normalColor, iconSize));
    m_btnNext->setIconSize(QSize(iconSize, iconSize));

    m_btnLast->setIcon(createIcon(g_svg_last_page, normalColor, iconSize));
    m_btnLast->setIconSize(QSize(iconSize, iconSize));
}

void PageBar::updateDisplay()
{
    if(m_state.total.has_value()) {
        m_labelPage->setText(QString("%1 / %2").arg(m_state.page).arg(m_state.pages()));
    } else {
        m_labelPage->setText(QString("%1 / ...").arg(m_state.page));
    }

    if(m_busy) {
        m_btnFirst->setEnabled(false);
        m_btnPrev->setEnabled(false);
        m_btnNext->setEnabled(false);
        m_btnLast->setEnabled(false);
    } else {
        m_btnFirst->setEnabled(m_state.canFirst());
        m_btnPrev->setEnabled(m_state.canPrev());
        m_btnNext->setEnabled(m_state.canNext());
        m_btnLast->setEnabled(m_state.canLast());
    }
}

} // namespace dtv::ui
