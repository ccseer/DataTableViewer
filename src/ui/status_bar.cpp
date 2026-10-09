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

    m_valueLabel->setTextFormat(Qt::PlainText);
    m_sorting = new QLabel(this);
    m_sorting->setTextFormat(Qt::RichText);
    m_sorting->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    m_sorting->setText(tr("Sorting...").toHtmlEscaped() + " <a href=\"cancel\">" +
                       tr("Cancel").toHtmlEscaped() + "</a>");
    layout->addWidget(m_sorting);
    connect(m_sorting, &QLabel::linkActivated, this, [this](const QString &link) {
        if(link == "cancel")
            emit cancelSortRequested();
    });

    m_info = new QLabel(this);
    m_info->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_info->setCursor(Qt::ArrowCursor);
    m_info->setToolTip("DataTableViewer");
    layout->addWidget(m_info, 0);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);
    m_progress->hide();
    layout->addWidget(m_progress);

    clear();
}

void StatusBar::setLoadInfo(int rowCount, int colCount, qint64 fileBytes, qint64 elapsedMs,
                            const QString &formatName, const QString &libraryCredit, bool truncated,
                            size_t totalRows)
{
    m_pagedMode = false;
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

void StatusBar::setPagedMode(bool paged)
{
    m_pagedMode = paged;
    updateDisplay();
}

void StatusBar::setPagedLoadInfo(int64_t firstRow, int64_t lastRow, std::optional<int64_t> total,
                                 int colCount, qint64 fileBytes, qint64 elapsedMs,
                                 const QString &formatName, const QString &libraryCredit)
{
    m_pagedMode = true;
    m_firstRow = firstRow;
    m_lastRow = lastRow;
    m_pagedTotal = total;
    m_colCount = colCount;
    m_fileBytes = fileBytes;
    m_elapsedMs = elapsedMs;
    m_formatName = formatName;
    m_libraryCredit = libraryCredit;
    m_hasLoadInfo = true;

    rebuildPagedTexts();
    if(!m_indexingError.isEmpty()) {
        setWarning(m_indexingError);
    }

    m_info->setToolTip(m_tooltipLines);
    repaintInfoIcon();
    if(!m_indexing) {
        m_progress->hide();
    }

    if(!m_indexingError.isEmpty()) {
        setValueText(tr("Indexing error: %1").arg(m_indexingError));
    } else if(m_indexing && !m_indexingText.isEmpty()) {
        setValueText(m_indexingText);
    } else {
        setValueText(m_summaryText);
    }
}

void StatusBar::updatePagedTotal(int64_t total)
{
    // Cleared before the guard: indexing can finish before the first page
    // reaches the status bar, and a flag that survives would resurrect a stale
    // "Loading records" line from restoreInfo() on every later page turn.
    const QString indexingText = m_indexingText;
    m_indexing = false;
    m_indexingText.clear();
    m_indexingError.clear();

    if(!m_pagedMode || !m_hasLoadInfo)
        return;
    m_pagedTotal = total;
    m_progress->hide();

    QString oldSummary = m_summaryText;
    rebuildPagedTexts();

    m_info->setToolTip(m_tooltipLines);

    // Replace only text this bar produced itself. A live cell selection owns
    // the value line, so finishing the index must not overwrite it.
    if(m_currentValueText.isEmpty() || m_currentValueText == oldSummary ||
       m_currentValueText == indexingText) {
        setValueText(m_summaryText);
    }
}

void StatusBar::setIndexingProgress(int64_t totalRows)
{
    // Same ownership rule as updatePagedTotal: progress ticks must not evict a
    // cell value the user just selected, because nothing restores it later.
    const bool showsOwnText = m_currentValueText.isEmpty() || m_currentValueText == m_summaryText ||
                              m_currentValueText == m_indexingText;

    m_indexing = true;
    m_indexingText = tr("Loading records: %L1...").arg(totalRows);
    m_progress->show();
    if(showsOwnText) {
        setValueText(m_indexingText);
    }
}

void StatusBar::setIndexingFailed(const QString &error)
{
    m_indexing = false;
    m_indexingText.clear();
    m_indexingError = error;
    m_progress->hide();
    // Without load info the warning would be appended to the previous file's
    // summary and leaked into the info tooltip as if it belonged to this one.
    if(m_hasLoadInfo) {
        setWarning(error);
    }
    setValueText(tr("Indexing error: %1").arg(error));
}

void StatusBar::rebuildPagedTexts()
{
    QString totalStr = m_pagedTotal.has_value() ? QString("%L1").arg(*m_pagedTotal) : "...";

    QStringList lines;
    lines << QString("Format: %1").arg(m_formatName);
    lines << QString("Rows: %L1-%L2 of %3").arg(m_firstRow).arg(m_lastRow).arg(totalStr);
    lines << QString("Columns: %1").arg(m_colCount);
    if(m_fileBytes > 0) {
        lines << QString("File size: %1").arg(fileSizeStr(m_fileBytes));
    }
    lines << QString("Load time: %1 ms").arg(m_elapsedMs);
    if(!m_libraryCredit.isEmpty()) {
        lines << QString("Library: %1").arg(m_libraryCredit);
    }
    m_tooltipLines = lines.join("\n");

    m_summaryText = QString("%1  ·  rows %L2-%L3 of %4  ·  %5 columns")
                        .arg(m_formatName)
                        .arg(m_firstRow)
                        .arg(m_lastRow)
                        .arg(totalStr)
                        .arg(m_colCount);

    // The summary and tooltip were just rebuilt without the warning, so the
    // next setWarning() has to be allowed to fold it back in.
    m_appliedWarning.clear();
}

void StatusBar::setWarning(const QString &warning)
{
    if(warning.isEmpty() || warning == m_appliedWarning)
        return;

    // The text this warning is folded into is rebuilt from scratch by
    // rebuildPagedTexts(), so a new warning replaces the previous one instead
    // of stacking on top of it. Reapplying the stored indexing error on every
    // page turn must not grow the summary or the tooltip.
    const QString oldSummary = m_summaryText;
    m_appliedWarning = warning;
    QString warnText = QString("  (Warning: %1)").arg(warning);
    m_summaryText += warnText;
    m_tooltipLines += "\n" + warnText;
    m_info->setToolTip(m_tooltipLines);

    // If we are currently showing the summary (no selection), update it immediately
    if(m_currentValueText.isEmpty() || m_currentValueText == oldSummary) {
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
        if(m_pagedMode) {
            if(m_matchCount == 0) {
                display = QString("[%1]  %2").arg(tr("no match on this page"), m_currentValueText);
            } else if(m_matchCount == 1) {
                display = QString("[%1]  %2").arg(tr("1 match on this page"), m_currentValueText);
            } else {
                // %n lets translators supply the target language plural forms.
                display = QString("[%1]  %2")
                              .arg(tr("%n matches on this page", nullptr, m_matchCount),
                                   m_currentValueText);
            }
        } else {
            display = QString("[Matches: %1]  %2").arg(m_matchCount).arg(m_currentValueText);
        }
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

void StatusBar::resetLoadInfo()
{
    m_indexing = false;
    m_indexingText.clear();
    m_indexingError.clear();
    m_summaryText.clear();
    m_tooltipLines.clear();
    m_appliedWarning.clear();
    m_hasLoadInfo = false;
    m_filterActive = false;
    m_pagedTotal.reset();
    m_firstRow = 0;
    m_lastRow = 0;
    m_colCount = 0;
    m_fileBytes = 0;
    m_elapsedMs = 0;
    m_formatName.clear();
    m_libraryCredit.clear();
}

void StatusBar::showLoading()
{
    resetLoadInfo();
    m_info->setPixmap(QPixmap());
    m_info->setText("Loading...");
    m_info->setToolTip("DataTableViewer");
    setValueText({});
    m_progress->show();
}

void StatusBar::hideLoading()
{
    m_progress->hide();
}

void StatusBar::restoreInfo()
{
    m_info->setText({});
    if(m_hasLoadInfo) {
        repaintInfoIcon();
        if(!m_indexingError.isEmpty()) {
            setValueText(tr("Indexing error: %1").arg(m_indexingError));
        } else if(m_indexing && !m_indexingText.isEmpty()) {
            setValueText(m_indexingText);
        } else {
            setValueText(m_summaryText);
        }
    } else {
        m_filterActive = false;
        m_info->setPixmap(QPixmap());
        m_info->setToolTip("DataTableViewer");
        setValueText({});
    }
    if(!m_indexing) {
        m_progress->hide();
    }
}

void StatusBar::updateTheme(bool dark, qreal dpr)
{
    Q_UNUSED(dark);
    m_dpr = dpr;
    setFixedHeight(qRound(26 * m_dpr));
    const int infoBox = qRound(24 * m_dpr);
    m_info->setFixedSize(infoBox, infoBox);
    m_progress->setMaximumWidth(qRound(80 * m_dpr));
    m_progress->setMaximumHeight(qRound(12 * m_dpr));
    if(auto *lay = qobject_cast<QHBoxLayout *>(layout()))
        lay->setContentsMargins(qRound(12 * m_dpr), 0, qRound(12 * m_dpr), 0);
    if(m_hasLoadInfo)
        repaintInfoIcon();
}

void StatusBar::setSorting(bool sorting)
{
    m_sorting->setVisible(sorting);
}

void StatusBar::clear()
{
    setSorting(false);
    resetLoadInfo();
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
