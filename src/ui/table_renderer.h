#pragma once

#include <QWidget>
#include <QPointer>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/table_data.h"
#include "core/table_source.h"

class QTableView;
class QHeaderView;
class QSettings;
class QAction;

namespace dtv {
namespace ui {

class TableModel;
class TableFilterProxy;

class TableRenderer : public QWidget {
    Q_OBJECT
public:
    explicit TableRenderer(QWidget *parent = nullptr);

    void setData(std::shared_ptr<const core::TableData> data);
    void setPageData(std::shared_ptr<const core::TableData> data,
                     std::vector<core::RefetchKey> keys = {},
                     std::vector<std::vector<bool>> clamped = {},
                     int64_t rowOffset = 0);
    void clear();

    void setPagedMode(bool paged);
    bool isPagedMode() const;
    int rowCount() const;
    TableModel *model() const;
    QHeaderView *horizontalHeader() const;

    bool isCellClamped(int modelRow, int modelCol) const;
    bool isSelectedCellClamped() const;
    void selectCell(int row, int col);
    void setCopyAction(QAction *action);
    void setShowRowIndex(bool show);
    bool showRowIndex() const { return m_showRowIndex; }
    void updateTheme(bool dark, qreal dpr);

    void setStateKey(const QString &key); // includes parser-format and table name
    void saveHeaderState(QSettings &settings) const;
    void restoreHeaderState(QSettings &settings);

    void setFilter(const QString &text, int columnScope);
    int filterMatchCount() const;

signals:
    void filterCountChanged(int matches);
    void requestFilter(const QString &text);
    void currentItemChanged(const QString &header, const QString &value);
    void pageUpRequested();
    void pageDownRequested();
    void refetchRowsRequested(uint64_t copyRequestId, bool isMarkdown,
                              const std::vector<std::pair<int, dtv::core::RefetchKey>> &rowKeys);
    void copyRefetchIncomplete(int failedRows);

public slots:
    void copyToClipboard();
    void copyAsMarkdown();
    void resizeColumnsToFit();
    void filterBySelection();
    void onRefetchRowsCompleted(uint64_t copyRequestId,
                                const std::vector<std::pair<int, dtv::core::RefetchResult>> &results);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void showContextMenu(const QPoint &pos);
    void onHeaderClicked(int column);

private:
    void setupView();
    void performCopy(bool isMarkdown);
    void updateVerticalHeaderWidth();

    QTableView *m_view = nullptr;
    TableModel *m_model = nullptr;
    TableFilterProxy *m_proxy = nullptr;
    QString m_stateKey;

    int m_lastSortCol = -1;
    Qt::SortOrder m_lastSortOrder = Qt::AscendingOrder;
    bool m_lastSortShown = false;
    bool m_pagedMode = false;
    bool m_showRowIndex = true;
    bool m_isDarkMode = false;
    qreal m_dpr = 1.0;
    QPointer<QAction> m_copyAction;

    std::vector<core::RefetchKey> m_pageKeys;
    std::vector<std::vector<bool>> m_pageClamped;

    struct PendingCopyCell {
        int visRow = 0;
        int visCol = 0;
        int modelRow = 0;
        int modelCol = 0;
        QString displayedText;
        QString headerText;
        bool clamped = false;
    };
    struct PendingCopy {
        uint64_t copyRequestId = 0;
        bool isMarkdown = false;
        std::vector<PendingCopyCell> cells;
    };

    uint64_t m_currentCopyRequestId = 0;
    std::optional<PendingCopy> m_pendingCopy;

    // Raw cell value: refetched full text for clamped cells, displayed text otherwise.
    QString rawCellText(const PendingCopyCell &cell,
                        const std::unordered_map<int, std::unordered_map<int, std::string>> &refetched) const;
    QString buildPlainText(const std::vector<PendingCopyCell> &cells,
                           const std::unordered_map<int, std::unordered_map<int, std::string>> &refetched) const;
    QString buildMarkdownText(const std::vector<PendingCopyCell> &cells,
                              const std::unordered_map<int, std::unordered_map<int, std::string>> &refetched) const;
};

} // namespace ui
} // namespace dtv
