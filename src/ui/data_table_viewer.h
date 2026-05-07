#pragma once

#include "seer/viewerbase.h"
#include "core/itable_parser.h"

#include <memory>

class QStackedLayout;
class QPushButton;

namespace dtv {
namespace ui {
class SearchBar;
class StatusBar;
class TableRenderer;
class TablePicker;
} // namespace ui
} // namespace dtv

class DataTableViewer : public ViewerBase {
    Q_OBJECT
public:
    explicit DataTableViewer(QWidget *parent = nullptr);
    ~DataTableViewer() override;

    QString name() const override
    {
        return "DataTableViewer";
    }
    QSize getContentSize() const override
    {
        return {960, 600};
    }
    void loadImpl(QBoxLayout *lay_content, QHBoxLayout *lay_ctrlbar) override;
    void updateDPR(qreal r) override;
    void updateTheme(int theme) override;

    void onCopyTriggered() override;

signals:
    void cancelRequested();

private slots:
    void onParseCompleted(std::shared_ptr<const dtv::core::TableParseResult> result,
                          const QString &tableName, int generation);
    void doLoadFile(const QString &path, const QString &tableName);

private:
    void init();
    void cancelPending();
    void reapplyStyles();
    QString makeKey(const QString &format, const QString &table) const;

    dtv::ui::SearchBar *m_search = nullptr;
    dtv::ui::StatusBar *m_status = nullptr;
    dtv::ui::TableRenderer *m_renderer = nullptr;
    dtv::ui::TablePicker *m_picker = nullptr;
    QStackedLayout *m_stack = nullptr;
    QPushButton *m_backBtn = nullptr;

    QString m_currentPath;
    int m_generation = 0;
    bool m_sqliteAvailable = false;
    bool m_isDarkMode = false;
    qreal m_dpr = 1.0;
};

// Plugin entry point
class DTVPlugin : public QObject, public ViewerPluginInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ViewerPluginInterface_iid FILE "../../bin/plugin.json")
    Q_INTERFACES(ViewerPluginInterface)
public:
    ViewerBase *createViewer(QWidget *parent = nullptr) override
    {
        return new DataTableViewer(parent);
    }
};
