#include "action_registry.h"

#include <QDebug>

#include <algorithm>

namespace dtv::ui {

namespace {
bool isModifierKey(Qt::Key key)
{
    switch(key) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Meta:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
        return true;
    default:
        return false;
    }
}

bool isValidKeySequence(const QKeySequence &seq)
{
    if(seq.isEmpty()) {
        return false;
    }
    for(int i = 0; i < seq.count(); ++i) {
        const Qt::Key key = seq[i].key();
        if(key == Qt::Key_unknown || key == 0 || isModifierKey(key)) {
            return false;
        }
    }
    return true;
}
} // namespace

ActionRegistry::ActionRegistry(QWidget *parentWidget)
    : QObject(nullptr), m_parentWidget(parentWidget)
{}

ActionRegistry::~ActionRegistry()
{
    for(auto &pair : m_actions) {
        if(pair.second) {
            if(m_parentWidget) {
                m_parentWidget->removeAction(pair.second);
            }
            delete pair.second.data();
        }
    }
}

void ActionRegistry::loadShortcuts(QSettings &settings)
{
    m_configuredValues.clear();

    settings.beginGroup("Shortcuts");
    const QStringList keys = settings.childKeys();
    for(const QString &key : keys) {
        m_configuredValues[key] = settings.value(key).toString().trimmed();
    }
    settings.endGroup();

    // Re-resolve shortcuts for all registered actions
    m_boundShortcuts.clear();
    for(const QString &id : m_order) {
        auto actIt = m_actions.find(id);
        if(actIt != m_actions.end() && actIt->second) {
            resolveAndApplyShortcut(id, actIt->second.data(), m_defaultShortcuts[id]);
        }
    }
}

void ActionRegistry::saveDefaultsIfMissing(QSettings &settings)
{
    settings.beginGroup("Shortcuts");
    for(const QString &id : m_order) {
        if(!settings.contains(id)) {
            auto it = m_defaultShortcuts.find(id);
            if(it != m_defaultShortcuts.end() && !it->second.isEmpty()) {
                settings.setValue(id, it->second.toString(QKeySequence::PortableText));
            }
        }
    }
    settings.endGroup();
}

QAction *ActionRegistry::registerAction(const ActionDescriptor &desc)
{
    if(desc.id.isEmpty()) {
        return nullptr;
    }

    auto existingIt = m_actions.find(desc.id);
    if(existingIt != m_actions.end()) {
        if(existingIt->second) {
            return existingIt->second.data();
        }
        // The action object was destroyed externally; drop the stale record so
        // the id can be registered again cleanly.
        m_actions.erase(existingIt);
        m_order.erase(std::remove(m_order.begin(), m_order.end(), desc.id), m_order.end());
    }

    QObject *actionParent = m_parentWidget ? static_cast<QObject *>(m_parentWidget) : this;
    auto *act = new QAction(desc.text, actionParent);
    act->setObjectName(desc.id);
    act->setCheckable(desc.checkable);
    act->setShortcutContext(Qt::WidgetWithChildrenShortcut);

    if(m_parentWidget) {
        m_parentWidget->addAction(act);
    }

    if(desc.onTriggered) {
        connect(act, &QAction::triggered, this, [fn = desc.onTriggered](bool) {
            fn();
        });
    }

    m_actions[desc.id] = act;
    m_defaultShortcuts[desc.id] = desc.defaultShortcut;
    m_order.push_back(desc.id);

    resolveAndApplyShortcut(desc.id, act, desc.defaultShortcut);

    return act;
}

QAction *ActionRegistry::registerAction(const QString &id, const QString &text,
                                        const QKeySequence &defaultShortcut,
                                        std::function<void()> onTriggered, bool checkable)
{
    ActionDescriptor desc;
    desc.id = id;
    desc.text = text;
    desc.defaultShortcut = defaultShortcut;
    desc.onTriggered = std::move(onTriggered);
    desc.checkable = checkable;
    return registerAction(desc);
}

void ActionRegistry::resolveAndApplyShortcut(const QString &id, QAction *act,
                                             const QKeySequence &defaultSeq)
{
    QKeySequence effective = defaultSeq;

    auto confIt = m_configuredValues.find(id);
    if(confIt != m_configuredValues.end() && !confIt->second.isEmpty()) {
        QKeySequence parsed = QKeySequence::fromString(confIt->second, QKeySequence::PortableText);
        if(!isValidKeySequence(parsed)) {
            parsed = QKeySequence::fromString(confIt->second, QKeySequence::NativeText);
        }
        if(isValidKeySequence(parsed)) {
            effective = parsed;
        } else {
            const QString warnKey = id + ":" + confIt->second;
            if(m_warnedInvalid.insert(warnKey).second) {
                qWarning() << "[ActionRegistry] Invalid shortcut" << confIt->second << "for action"
                           << id << "- falling back to default";
            }
        }
    }

    m_effectiveShortcuts[id] = effective;
    act->setShortcut(effective);

    // Duplicate key detection and warning
    if(!effective.isEmpty()) {
        const QString seqStr = effective.toString(QKeySequence::PortableText);
        if(!seqStr.isEmpty()) {
            auto boundIt = m_boundShortcuts.find(seqStr);
            if(boundIt != m_boundShortcuts.end() && boundIt->second != id) {
                const QString warnPair = boundIt->second + ":" + id + ":" + seqStr;
                if(m_warnedDuplicates.insert(warnPair).second) {
                    qWarning() << "[ActionRegistry] Duplicate shortcut" << seqStr
                               << "between action" << boundIt->second << "and action" << id;
                }
            } else {
                m_boundShortcuts[seqStr] = id;
            }
        }
    }
}

QAction *ActionRegistry::action(const QString &id) const
{
    auto it = m_actions.find(id);
    return (it != m_actions.end()) ? it->second.data() : nullptr;
}

QKeySequence ActionRegistry::shortcut(const QString &id) const
{
    auto it = m_effectiveShortcuts.find(id);
    return (it != m_effectiveShortcuts.end()) ? it->second : QKeySequence();
}

QString ActionRegistry::shortcutNativeText(const QString &id) const
{
    QKeySequence seq = shortcut(id);
    return seq.isEmpty() ? QString() : seq.toString(QKeySequence::NativeText);
}

const std::vector<QString> &ActionRegistry::registeredIds() const
{
    return m_order;
}

} // namespace dtv::ui
