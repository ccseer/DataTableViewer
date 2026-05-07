#include "table_picker.h"
#include "style_assets.h"
#include <QVBoxLayout>
#include <QListWidget>
#include <QLineEdit>
#include <QLabel>
#include <QListWidgetItem>
#include <QDebug>

namespace dtv {
namespace ui {

TablePicker::TablePicker(QWidget *parent) : QWidget(parent)
{
    setupUi();
}

void TablePicker::setupUi()
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(32, 28, 32, 28);
    lay->setSpacing(14);
    m_layout = lay;

    m_title = new QLabel("Tables", this);
    m_title->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_title->setStyleSheet("font-size: 15px; font-weight: 600;");
    lay->addWidget(m_title);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText("Filter tables...");
    m_filter->setClearButtonEnabled(true);
    m_filter->hide(); // Shown only if > 10 tables
    lay->addWidget(m_filter);

    m_list = new QListWidget(this);
    m_list->setSpacing(2);
    m_list->setIconSize(QSize(20, 20));
    m_list->setCursor(Qt::PointingHandCursor);
    m_list->setFocusPolicy(Qt::StrongFocus);
    lay->addWidget(m_list);

    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        emit tableSelected(item->text());
    });
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        emit tableSelected(item->text());
    });

    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString &text) {
        for(int i = 0; i < m_list->count(); ++i) {
            auto *item = m_list->item(i);
            item->setHidden(!item->text().contains(text, Qt::CaseInsensitive));
        }
    });
}

void TablePicker::setTables(const QStringList &tables)
{
    m_allTables = tables;
    m_list->clear();
    m_filter->clear();

    m_filter->setVisible(tables.size() > 10);
    const int iconSize = qRound(20 * m_dpr);
    const QIcon tableIcon =
        dtv::ui::createIcon(dtv::ui::g_svg_table, QColor(dtv::ui::Colors::Accent), iconSize);

    for(const auto &name : tables) {
        auto *item = new QListWidgetItem(tableIcon, name, m_list);
        m_list->addItem(item);
    }
}

void TablePicker::updateTheme(bool dark, qreal dpr)
{
    m_isDarkMode = dark;
    m_dpr = dpr;

    if(m_layout) {
        m_layout->setContentsMargins(qRound(32 * dpr), qRound(28 * dpr), qRound(32 * dpr),
                                     qRound(28 * dpr));
        m_layout->setSpacing(qRound(14 * dpr));
    }

    const QColor textColor(dark ? dtv::ui::Colors::DarkText : dtv::ui::Colors::LightText);
    const QColor dimColor(dark ? dtv::ui::Colors::DarkTextDim : dtv::ui::Colors::LightTextDim);
    const QColor inputColor(dark ? dtv::ui::Colors::DarkInput : dtv::ui::Colors::LightInput);
    const QColor borderColor(dark ? dtv::ui::Colors::DarkBorder : dtv::ui::Colors::LightBorder);
    const QColor accentColor(dtv::ui::Colors::Accent);
    const QString selectedBg = QString("rgba(%1, %2, %3, %4)")
                                   .arg(accentColor.red())
                                   .arg(accentColor.green())
                                   .arg(accentColor.blue())
                                   .arg(dark ? 56 : 34);

    m_title->setStyleSheet(QString("font-size: %1px; font-weight: 600; color: %2;")
                               .arg(qRound(15 * dpr))
                               .arg(textColor.name()));

    const int iconSize = qRound(20 * dpr);
    const QIcon tableIcon = dtv::ui::createIcon(dtv::ui::g_svg_table, accentColor, iconSize);
    m_list->setIconSize(QSize(iconSize, iconSize));
    for(int i = 0; i < m_list->count(); ++i) {
        if(auto *item = m_list->item(i))
            item->setIcon(tableIcon);
    }

    m_filter->setFixedHeight(qRound(30 * dpr));
    m_filter->setStyleSheet(
        QString("QLineEdit { background-color: %1; border: 1px solid %2; border-radius: %3px; "
                "color: %4; padding: %5px %6px; selection-background-color: %7; "
                "placeholder-text-color: %8; }"
                "QLineEdit:focus { border-color: %7; }")
            .arg(inputColor.name())
            .arg(borderColor.name())
            .arg(qRound(6 * dpr))
            .arg(textColor.name())
            .arg(qRound(4 * dpr))
            .arg(qRound(9 * dpr))
            .arg(accentColor.name())
            .arg(dimColor.name()));

    m_list->setStyleSheet(
        QString("QListWidget { background: transparent; border: none; font-size: %1px; color: %2; "
                "outline: none; }"
                "QListWidget::item { padding: %3px %4px; border-radius: %5px; }"
                "QListWidget::item:hover { background-color: rgba(128, 128, 128, 32); }"
                "QListWidget::item:selected { background-color: %6; color: %2; }")
            .arg(qRound(14 * dpr))
            .arg(textColor.name())
            .arg(qRound(7 * dpr))
            .arg(qRound(9 * dpr))
            .arg(qRound(5 * dpr))
            .arg(selectedBg));
}

void TablePicker::clear()
{
    m_allTables.clear();
    m_list->clear();
    m_filter->clear();
}

} // namespace ui
} // namespace dtv
