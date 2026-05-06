#pragma once

#include <QWidget>

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
    void setValueText(const QString &text);
    QString text() const;
    void showLoading();
    void showFilterNoHits(const QString &text);
    void restoreInfo();
    void updateTheme(bool dark, qreal dpr);
    void clear();

private:
    void repaintInfoIcon();

    QLabel *m_info = nullptr;
    QLabel *m_valueLabel = nullptr;
    QProgressBar *m_progress = nullptr;

    bool m_isDarkMode = false;
    qreal m_dpr = 1.0;
    bool m_hasLoadInfo = false;

    // Stored for theme repaint
    QString m_tooltipLines;
};

} // namespace ui
} // namespace dtv
