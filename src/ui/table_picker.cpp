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
    lay->setContentsMargins(40, 40, 40, 40);
    lay->setSpacing(20);

    m_title = new QLabel("Select a table to preview:", this);
    m_title->setStyleSheet("font-size: 16px; font-weight: bold;");
    lay->addWidget(m_title, 0, Qt::AlignCenter);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText("Filter tables...");
    m_filter->setClearButtonEnabled(true);
    m_filter->hide(); // Shown only if > 10 tables
    lay->addWidget(m_filter);

    m_list = new QListWidget(this);
    m_list->setSpacing(4);
    m_list->setIconSize(QSize(20, 20));
    m_list->setCursor(Qt::PointingHandCursor);
    m_list->setStyleSheet(
        "QListWidget { background: transparent; border: none; font-size: 14px; }"
        "QListWidget::item { padding: 8px; border-radius: 4px; }"
        "QListWidget::item:hover { background-color: rgba(128, 128, 128, 30); }"
        "QListWidget::item:selected { background-color: rgba(2, 136, 209, 40); color: inherit; }");
    lay->addWidget(m_list);

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

    QIcon tableIcon = dtv::ui::createIcon(dtv::ui::g_svg_table, QColor(dtv::ui::Colors::Accent));

    for(const auto &name : tables) {
        auto *item = new QListWidgetItem(tableIcon, name, m_list);
        m_list->addItem(item);
    }
}

void TablePicker::updateTheme(bool dark, qreal dpr)
{
    // Re-create icons if needed, but QListWidget items need refresh
    if(!m_allTables.isEmpty()) {
        setTables(m_allTables);
    }

    QColor textColor(dark ? dtv::ui::Colors::DarkText : dtv::ui::Colors::LightText);
    m_title->setStyleSheet(QString("font-size: %1px; font-weight: bold; color: %2;")
                               .arg(qRound(16 * dpr))
                               .arg(textColor.name()));
}

void TablePicker::clear()
{
    m_allTables.clear();
    m_list->clear();
    m_filter->clear();
}

} // namespace ui
} // namespace dtv
