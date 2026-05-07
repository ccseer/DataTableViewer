#include "status_bar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QDebug>

#include "style_assets.h"

#define qprintt qDebug() << "[StatusBar]"

namespace {

QString fileSizeStr(qint64 bytes)
{
    if(bytes < 1024)
        return QString::number(bytes) + " B";
    if(bytes < 1024 * 1024)
        return QString::number(bytes / 1024.0, 'f', 1) + " KB";
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 2) + " MB";
}

} // namespace

namespace dtv {
namespace ui {

StatusBar::StatusBar(QWidget *parent) : QWidget(parent)
{
    setObjectName("btmBar");

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 2, 12, 2);
    layout->setSpacing(4);

    m_valueLabel = new QLabel(this);
    m_valueLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_valueLabel, 1);

    m_info = new QLabel(this);
    m_info->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_info->setCursor(Qt::ArrowCursor);
    m_info->setToolTip("DataTableViewer");
    layout->addWidget(m_info, 0);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);
    m_progress->setMaximumWidth(80);
    m_progress->setMaximumHeight(12);
    m_progress->hide();
    layout->addWidget(m_progress);

    clear();
}

void StatusBar::setLoadInfo(int rowCount, int colCount, qint64 fileBytes, qint64 elapsedMs,
                            const QString &formatName, const QString &libraryCredit, bool truncated,
                            size_t totalRows)
{
    QStringList lines;
    lines << QString("Format: %1").arg(formatName);
    if(truncated) {
        if(totalRows > 0) {
            lines << QString("Rows: Showing %L1 of %L2").arg(rowCount).arg(totalRows);
        } else {
            lines << QString("Rows: %L1+").arg(rowCount);
        }
    } else {
        lines << QString("Rows: %L1").arg(rowCount);
    }
    lines << QString("Columns: %1").arg(colCount);
    if(fileBytes > 0) {
        lines << QString("File size: %1").arg(fileSizeStr(fileBytes));
    }
    lines << QString("Load time: %1 ms").arg(elapsedMs);
    if(!libraryCredit.isEmpty())
        lines << QString("Library: %1").arg(libraryCredit);

    m_tooltipLines = lines.join("\n");
    m_hasLoadInfo = true;

    // Build a summary for restoration when no item is selected
    if(truncated) {
        if(totalRows > 0) {
            m_summaryText = QString("%1  ·  Showing %L2 of %L3 rows  ·  %4 columns")
                                .arg(formatName)
                                .arg(rowCount)
                                .arg(totalRows)
                                .arg(colCount);
        } else {
            m_summaryText = QString("%1  ·  %L2+ rows  ·  %3 columns")
                                .arg(formatName)
                                .arg(rowCount)
                                .arg(colCount);
        }
    } else {
        m_summaryText = QString("%1  ·  %L2 rows  ·  %3 columns  ·  %4 ms")
                            .arg(formatName)
                            .arg(rowCount)
                            .arg(colCount)
                            .arg(elapsedMs);
    }

    m_info->setToolTip(m_tooltipLines);
    repaintInfoIcon();
    m_progress->hide();

    setValueText(m_summaryText);
}

void StatusBar::setWarning(const QString &warning)
{
    if(warning.isEmpty())
        return;

    QString warnText = QString("  (Warning: %1)").arg(warning);
    m_summaryText += warnText;
    m_tooltipLines += "\n" + warnText;
    m_info->setToolTip(m_tooltipLines);

    // If we are currently showing the summary (no selection), update it immediately
    if(m_currentValueText.isEmpty() ||
       m_currentValueText == m_summaryText.left(m_summaryText.length() - warnText.length())) {
        setValueText(m_summaryText);
    }
}

void StatusBar::setFilterMatchCount(int count, bool active)
{
    m_matchCount = count;
    m_filterActive = active;
    updateDisplay();
}

void StatusBar::setValueText(const QString &text)
{
    m_currentValueText = text;
    updateDisplay();
}

void StatusBar::updateDisplay()
{
    QString display = m_currentValueText;
    if(m_filterActive) {
        display = QString("[Matches: %1]  %2").arg(m_matchCount).arg(m_currentValueText);
    }

    if(display.isEmpty()) {
        m_valueLabel->clear();
        m_valueLabel->setToolTip({});
    } else {
        m_valueLabel->setText(display);
        m_valueLabel->setToolTip(display);
    }
}

QString StatusBar::text() const
{
    return m_valueLabel->text();
}

void StatusBar::showLoading()
{
    m_hasLoadInfo = false;
    m_filterActive = false;
    m_info->setPixmap(QPixmap());
    m_info->setText("Loading...");
    m_info->setToolTip("DataTableViewer");
    setValueText({});
    m_progress->show();
}

void StatusBar::showFilterNoHits(const QString &text)
{
    m_info->setPixmap(QPixmap());
    m_info->setText(QString("No matches for \"%1\"").arg(text));
    m_info->setToolTip(QString("No matches for \"%1\"").arg(text));
}

void StatusBar::restoreInfo()
{
    m_info->setText({});
    if(m_hasLoadInfo) {
        repaintInfoIcon();
        setValueText(m_summaryText);
    } else {
        m_filterActive = false;
        m_info->setPixmap(QPixmap());
        m_info->setToolTip("DataTableViewer");
        setValueText({});
    }
    m_progress->hide();
}

void StatusBar::updateTheme(bool dark, qreal dpr)
{
    m_isDarkMode = dark;
    m_dpr = dpr;
    setFixedHeight(qRound(26 * m_dpr));
    const int infoBox = qRound(24 * m_dpr);
    m_info->setFixedSize(infoBox, infoBox);
    if(auto *lay = qobject_cast<QHBoxLayout *>(layout()))
        lay->setContentsMargins(qRound(12 * m_dpr), 0, qRound(12 * m_dpr), 0);
    if(m_hasLoadInfo)
        repaintInfoIcon();
}

void StatusBar::clear()
{
    m_hasLoadInfo = false;
    m_filterActive = false;
    m_info->setPixmap(QPixmap());
    m_info->clear();
    m_info->setToolTip("DataTableViewer");
    setValueText({});
    m_progress->hide();
}

void StatusBar::repaintInfoIcon()
{
    if(!m_hasLoadInfo)
        return;

    using namespace dtv::ui;

    QColor iconColor(Colors::Accent);

    int iconSize = qRound(20 * m_dpr);
    QIcon icon = dtv::ui::createMultiStateIcon(g_svg_info, iconColor, iconSize);
    m_info->setPixmap(icon.pixmap(iconSize, iconSize));
    m_info->setToolTip(m_tooltipLines);
}

} // namespace ui
} // namespace dtv
