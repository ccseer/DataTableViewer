#include "search_bar.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QTimer>
#include <QDebug>

#include "style_assets.h"

#define qprintt qDebug() << "[SearchBar]"

namespace dtv {
namespace ui {

SearchBar::SearchBar(QWidget *parent) : QWidget(parent)
{
    setObjectName("topBar");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 6, 12, 6);
    layout->setSpacing(10);

    m_edit = new QLineEdit(this);
    m_edit->setPlaceholderText("Filter current view...");
    m_edit->setClearButtonEnabled(true);
    layout->addWidget(m_edit, 1);

    m_searchIcon = new QLabel(m_edit);
    m_searchIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_searchIcon->setAlignment(Qt::AlignCenter);

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(150);

    connect(m_edit, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_timer->start();
    });

    connect(m_timer, &QTimer::timeout, this, [this] {
        emit filterChanged(m_edit->text());
    });

    connect(m_edit, &QLineEdit::returnPressed, this, [this] {
        if(m_edit->text().isEmpty())
            return;
        emit nextMatch();
    });

    m_edit->installEventFilter(this);
    updateDPR(1.0);
}

QString SearchBar::text() const
{
    return m_edit->text();
}

void SearchBar::clear()
{
    m_edit->clear();
}

void SearchBar::updateDPR(qreal r)
{
    m_dpr = r;
    if(auto *lay = qobject_cast<QHBoxLayout *>(layout())) {
        lay->setContentsMargins(qRound(9 * r), 0, qRound(9 * r), 0);
        lay->setSpacing(qRound(10 * r));
    }
    const int height = qRound(30 * r);
    const int iconBox = qRound(20 * r);
    const int iconGap = qRound(2 * r);
    m_edit->setFixedHeight(height);
    m_edit->setTextMargins(iconBox + iconGap, 0, qRound(8 * r), 0);
    m_searchIcon->setFixedSize(iconBox, height);
    refreshIcon();
    positionIcon();
}

void SearchBar::updateTheme(bool dark)
{
    m_isDarkMode = dark;
    refreshIcon();
}

bool SearchBar::eventFilter(QObject *obj, QEvent *event)
{
    if(obj == m_edit) {
        if(event->type() == QEvent::Resize || event->type() == QEvent::Move)
            positionIcon();
    }

    if(obj == m_edit && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if(keyEvent->key() == Qt::Key_Return && keyEvent->modifiers() & Qt::ShiftModifier) {
            if(!m_edit->text().isEmpty())
                emit prevMatch();
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

void SearchBar::refreshIcon()
{
    using namespace dtv::ui;
    const QColor iconColor(m_isDarkMode ? Colors::DarkText : Colors::LightText);
    m_searchIcon->setPixmap(createIcon(g_svg_search, iconColor, qRound(16 * m_dpr))
                                .pixmap(qRound(16 * m_dpr), qRound(16 * m_dpr)));
}

void SearchBar::positionIcon()
{
    if(!m_searchIcon || !m_edit)
        return;

    const int y = (m_edit->height() - m_searchIcon->height()) / 2;
    m_searchIcon->move(qRound(2 * m_dpr), y);
}

} // namespace ui
} // namespace dtv
