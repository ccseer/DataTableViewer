#pragma once

#include <QAction>
#include <QKeySequence>
#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QString>
#include <QWidget>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dtv::ui {

struct ActionDescriptor {
    QString id;
    QString text;
    QKeySequence defaultShortcut;
    std::function<void()> onTriggered = nullptr;
    bool checkable = false;
};

class ActionRegistry : public QObject {
    Q_OBJECT
public:
    explicit ActionRegistry(QWidget *parentWidget);
    ~ActionRegistry() override;

    // Load or reload configured shortcuts from settings [Shortcuts] section
    void loadShortcuts(QSettings &settings);

    // Persist compiled default shortcuts to settings [Shortcuts] section if missing
    void saveDefaultsIfMissing(QSettings &settings);

    // Register an action with the registry
    QAction *registerAction(const ActionDescriptor &desc);

    // Convenience registration overload
    QAction *registerAction(const QString &id, const QString &text,
                            const QKeySequence &defaultShortcut,
                            std::function<void()> onTriggered = nullptr, bool checkable = false);

    // Accessors
    QAction *action(const QString &id) const;
    QKeySequence shortcut(const QString &id) const;
    QString shortcutNativeText(const QString &id) const;
    const std::vector<QString> &registeredIds() const;

private:
    void resolveAndApplyShortcut(const QString &id, QAction *act, const QKeySequence &defaultSeq);

    // QPointer so a registry owned by the widget's QObject tree never touches
    // the widget after its QWidget destructor has already run.
    QPointer<QWidget> m_parentWidget;
    std::unordered_map<QString, QPointer<QAction>> m_actions;
    std::unordered_map<QString, QKeySequence> m_defaultShortcuts;
    std::unordered_map<QString, QKeySequence> m_effectiveShortcuts;
    std::vector<QString> m_order;
    std::unordered_map<QString, QString> m_configuredValues;
    std::unordered_map<QString, QString> m_boundShortcuts; // keySequence string -> action id
    std::unordered_set<QString> m_warnedDuplicates;
    std::unordered_set<QString> m_warnedInvalid;
};

} // namespace dtv::ui
