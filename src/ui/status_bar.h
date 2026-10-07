#pragma once

#include <QWidget>
#include <optional>

class QLabel;
class QProgressBar;

namespace dtv {
namespace ui {

class StatusBar : public QWidget {
    Q_OBJECT
public:
    explicit StatusBar(QWidget *parent = nullptr);

    void setLoadInfo(int rowCount, int colCount, qint64 fileBytes, qint64 elapsedMs,
                     const QString &formatName, const QString &libraryCredit,
                     bool truncated = false, size_t totalRows = 0);
    void setPagedLoadInfo(int64_t firstRow, int64_t lastRow, std::optional<int64_t> total,
                          int colCount, qint64 fileBytes, qint64 elapsedMs,
                          const QString &formatName, const QString &libraryCredit);
    void updatePagedTotal(int64_t total);
    void setPagedMode(bool paged);
    void setWarning(const QString &warning);
    void setFilterMatchCount(int count, bool active);
    void setValueText(const QString &text);
    QString text() const;
    void showLoading();
    void restoreInfo();
    void updateTheme(bool dark, qreal dpr);
    void clear();
    void setSorting(bool sorting);

signals:
    void cancelSortRequested();

private:
    void repaintInfoIcon();
    void updateDisplay();
    void rebuildPagedTexts();

    QLabel *m_valueLabel = nullptr;
    QLabel *m_info = nullptr;
    QLabel *m_sorting = nullptr;
    QProgressBar *m_progress = nullptr;

    bool m_isDarkMode = false;
    qreal m_dpr = 1.0;
    QString m_summaryText;
    QString m_currentValueText;
    int m_matchCount = 0;
    bool m_filterActive = false;
    bool m_hasLoadInfo = false;

    bool m_pagedMode = false;
    int64_t m_firstRow = 0;
    int64_t m_lastRow = 0;
    std::optional<int64_t> m_pagedTotal;
    int m_colCount = 0;
    qint64 m_fileBytes = 0;
    qint64 m_elapsedMs = 0;
    QString m_formatName;
    QString m_libraryCredit;

    // Stored for theme repaint
    QString m_tooltipLines;
};

} // namespace ui
} // namespace dtv
