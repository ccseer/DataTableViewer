#include <QtTest>
#include <QTemporaryDir>
#include <QSettings>
#include <QWidget>
#include <QAction>
#include "ui/action_registry.h"

class TestActionRegistry : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_tempDir;

private slots:
    void initTestCase()
    {
        m_tempDir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_tempDir->isValid());
    }

    void cleanupTestCase()
    {
        m_tempDir.reset();
    }

    void testDefaultShortcutsApplied()
    {
        QWidget widget;
        dtv::ui::ActionRegistry registry(&widget);

        bool triggered = false;
        QAction *act = registry.registerAction("DataTableViewer.find", "Find", QKeySequence::Find, [&]() {
            triggered = true;
        });

        QVERIFY(act != nullptr);
        QCOMPARE(registry.shortcut("DataTableViewer.find"), QKeySequence(QKeySequence::Find));
        QCOMPARE(act->shortcut(), QKeySequence(QKeySequence::Find));
        QCOMPARE(act->shortcutContext(), Qt::WidgetWithChildrenShortcut);

        act->trigger();
        QVERIFY(triggered);
    }

    void testConfiguredShortcutsFromSettings()
    {
        QString iniPath = m_tempDir->filePath("test_shortcuts.ini");
        {
            QSettings settings(iniPath, QSettings::IniFormat);
            settings.beginGroup("Shortcuts");
            settings.setValue("DataTableViewer.find", "Ctrl+Shift+F");
            settings.setValue("DataTableViewer.copy", "Alt+C");
            settings.endGroup();
            settings.sync();
        }

        QWidget widget;
        dtv::ui::ActionRegistry registry(&widget);

        QSettings settings(iniPath, QSettings::IniFormat);
        registry.loadShortcuts(settings);

        QAction *findAct = registry.registerAction("DataTableViewer.find", "Find", QKeySequence::Find);
        QAction *copyAct = registry.registerAction("DataTableViewer.copy", "Copy", QKeySequence::Copy);

        QCOMPARE(findAct->shortcut(), QKeySequence("Ctrl+Shift+F"));
        QCOMPARE(copyAct->shortcut(), QKeySequence("Alt+C"));
        QCOMPARE(registry.shortcut("DataTableViewer.find"), QKeySequence("Ctrl+Shift+F"));
        QCOMPARE(registry.shortcut("DataTableViewer.copy"), QKeySequence("Alt+C"));
    }

    void testMalformedOrEmptyShortcutFallsBackToDefault()
    {
        QString iniPath = m_tempDir->filePath("test_malformed.ini");
        {
            QSettings settings(iniPath, QSettings::IniFormat);
            settings.beginGroup("Shortcuts");
            settings.setValue("DataTableViewer.action1", "");
            settings.setValue("DataTableViewer.action2", "   ");
            settings.setValue("DataTableViewer.action3", "Ctrl+");
            settings.setValue("DataTableViewer.action4", "NotAValidKey");
            settings.setValue("DataTableViewer.action5", "Ctrl+InvalidKeyName");
            settings.setValue("DataTableViewer.action6", "Ctrl+Alt");
            settings.endGroup();
            settings.sync();
        }

        QWidget widget;
        dtv::ui::ActionRegistry registry(&widget);

        QSettings settings(iniPath, QSettings::IniFormat);
        registry.loadShortcuts(settings);

        QAction *act1 = registry.registerAction("DataTableViewer.action1", "Action 1", QKeySequence("Ctrl+1"));
        QAction *act2 = registry.registerAction("DataTableViewer.action2", "Action 2", QKeySequence("Ctrl+2"));
        QAction *act3 = registry.registerAction("DataTableViewer.action3", "Action 3", QKeySequence("Ctrl+3"));
        QAction *act4 = registry.registerAction("DataTableViewer.action4", "Action 4", QKeySequence("Ctrl+4"));
        QAction *act5 = registry.registerAction("DataTableViewer.action5", "Action 5", QKeySequence("Ctrl+5"));
        QAction *act6 = registry.registerAction("DataTableViewer.action6", "Action 6", QKeySequence("Ctrl+6"));

        // All must fall back to their compiled defaults
        QCOMPARE(act1->shortcut(), QKeySequence("Ctrl+1"));
        QCOMPARE(act2->shortcut(), QKeySequence("Ctrl+2"));
        QCOMPARE(act3->shortcut(), QKeySequence("Ctrl+3"));
        QCOMPARE(act4->shortcut(), QKeySequence("Ctrl+4"));
        QCOMPARE(act5->shortcut(), QKeySequence("Ctrl+5"));
        QCOMPARE(act6->shortcut(), QKeySequence("Ctrl+6"));
        QCOMPARE(registry.shortcut("DataTableViewer.action1"), QKeySequence("Ctrl+1"));
        QCOMPARE(registry.shortcut("DataTableViewer.action2"), QKeySequence("Ctrl+2"));
        QCOMPARE(registry.shortcut("DataTableViewer.action3"), QKeySequence("Ctrl+3"));
        QCOMPARE(registry.shortcut("DataTableViewer.action4"), QKeySequence("Ctrl+4"));
        QCOMPARE(registry.shortcut("DataTableViewer.action5"), QKeySequence("Ctrl+5"));
        QCOMPARE(registry.shortcut("DataTableViewer.action6"), QKeySequence("Ctrl+6"));
    }

    void testDuplicateShortcutDetection()
    {
        QWidget widget;
        dtv::ui::ActionRegistry registry(&widget);

        // Register two distinct actions with the same shortcut sequence
        QAction *act1 = registry.registerAction("DataTableViewer.actionA", "Action A", QKeySequence("Ctrl+K"));
        QAction *act2 = registry.registerAction("DataTableViewer.actionB", "Action B", QKeySequence("Ctrl+K"));

        QVERIFY(act1 != nullptr);
        QVERIFY(act2 != nullptr);
        QCOMPARE(act1->shortcut(), QKeySequence("Ctrl+K"));
        QCOMPARE(act2->shortcut(), QKeySequence("Ctrl+K"));
    }

    void testShortcutNativeTextFormat()
    {
        QWidget widget;
        dtv::ui::ActionRegistry registry(&widget);

        registry.registerAction("DataTableViewer.test", "Test", QKeySequence("Ctrl+Alt+T"));
        QString nativeText = registry.shortcutNativeText("DataTableViewer.test");
        QVERIFY(!nativeText.isEmpty());
        QCOMPARE(nativeText, QKeySequence("Ctrl+Alt+T").toString(QKeySequence::NativeText));
    }

    void testNativeTextConfiguredShortcutFallback()
    {
        QString iniPath = m_tempDir->filePath("test_native_fallback.ini");
        const QString nativeSeq = QKeySequence(Qt::ControlModifier | Qt::Key_Home).toString(QKeySequence::NativeText);
        {
            QSettings settings(iniPath, QSettings::IniFormat);
            settings.beginGroup("Shortcuts");
            settings.setValue("DataTableViewer.pageFirst", nativeSeq);
            settings.endGroup();
            settings.sync();
        }

        QWidget widget;
        dtv::ui::ActionRegistry registry(&widget);

        QSettings settings(iniPath, QSettings::IniFormat);
        registry.loadShortcuts(settings);

        QAction *act = registry.registerAction("DataTableViewer.pageFirst", "First page",
                                               QKeySequence(Qt::ControlModifier | Qt::Key_Home));
        QCOMPARE(act->shortcut(), QKeySequence(Qt::ControlModifier | Qt::Key_Home));
        QCOMPARE(registry.shortcut("DataTableViewer.pageFirst"), QKeySequence(Qt::ControlModifier | Qt::Key_Home));
    }

    void testSaveDefaultsIfMissing()
    {
        QString iniPath = m_tempDir->filePath("test_seed_defaults.ini");
        // Pre-create file with one existing custom shortcut
        {
            QSettings settings(iniPath, QSettings::IniFormat);
            settings.beginGroup("Shortcuts");
            settings.setValue("DataTableViewer.find", "Ctrl+H");
            settings.endGroup();
            settings.sync();
        }

        QWidget widget;
        dtv::ui::ActionRegistry registry(&widget);

        QSettings settings(iniPath, QSettings::IniFormat);
        registry.loadShortcuts(settings);

        registry.registerAction("DataTableViewer.find", "Find", QKeySequence::Find);
        registry.registerAction("DataTableViewer.copy", "Copy", QKeySequence::Copy);

        // Save defaults
        registry.saveDefaultsIfMissing(settings);
        settings.sync();

        // Verify settings on disk
        QSettings checkSettings(iniPath, QSettings::IniFormat);
        checkSettings.beginGroup("Shortcuts");
        // Custom shortcut must be preserved
        QCOMPARE(checkSettings.value("DataTableViewer.find").toString(), QString("Ctrl+H"));
        // Missing shortcut must be seeded with portable text
        QCOMPARE(checkSettings.value("DataTableViewer.copy").toString(),
                 QKeySequence(QKeySequence::Copy).toString(QKeySequence::PortableText));
        checkSettings.endGroup();
    }

    void testAdditiveSettingsLoad()
    {
        QString iniPath = m_tempDir->filePath("test_legacy_additive.ini");
        {
            QSettings settings(iniPath, QSettings::IniFormat);
            settings.setValue("page_rows", 250);
            settings.beginGroup("TablePlugin/CSV/HeaderState");
            settings.setValue("geom", "mock_geometry");
            settings.endGroup();
            settings.sync();
        }

        QWidget widget;
        dtv::ui::ActionRegistry registry(&widget);

        QSettings settings(iniPath, QSettings::IniFormat);
        registry.loadShortcuts(settings);

        QAction *findAct = registry.registerAction("DataTableViewer.find", "Find", QKeySequence::Find);
        QCOMPARE(findAct->shortcut(), QKeySequence(QKeySequence::Find));

        // Pre-existing keys remain untouched
        QCOMPARE(settings.value("page_rows").toInt(), 250);
        settings.beginGroup("TablePlugin/CSV/HeaderState");
        QCOMPARE(settings.value("geom").toString(), QString("mock_geometry"));
        settings.endGroup();
    }

    void testActionTextIndependence()
    {
        QWidget widget;
        dtv::ui::ActionRegistry registry(&widget);

        // Action text is Chinese / localized, while ID remains standard
        QAction *act = registry.registerAction("DataTableViewer.find", QString::fromUtf8("查找"), QKeySequence::Find);
        QCOMPARE(act->text(), QString::fromUtf8("查找"));
        QCOMPARE(act->objectName(), QString("DataTableViewer.find"));
        QCOMPARE(registry.shortcut("DataTableViewer.find"), QKeySequence(QKeySequence::Find));
    }

    void testTeardownWithNullParent()
    {
        QPointer<QAction> actTracker;
        {
            dtv::ui::ActionRegistry registry(nullptr);
            QAction *act = registry.registerAction("DataTableViewer.nullTest", "Null Parent", QKeySequence("Ctrl+N"));
            QVERIFY(act != nullptr);
            actTracker = act;
            QVERIFY(!actTracker.isNull());
        }
        // Action should have been cleanly destroyed along with registry
        QVERIFY(actTracker.isNull());
    }

    void testCleanTeardownWithParentWidget()
    {
        QWidget widget;
        QPointer<QAction> actTracker;
        {
            dtv::ui::ActionRegistry registry(&widget);
            QAction *act = registry.registerAction("DataTableViewer.parentTest", "Parent Test", QKeySequence("Ctrl+P"));
            QVERIFY(act != nullptr);
            actTracker = act;
            QVERIFY(widget.actions().contains(act));
        }
        // Action destroyed and removed from parent widget
        QVERIFY(actTracker.isNull());
        QVERIFY(widget.actions().isEmpty());
    }

    void testReadOnlySettingsFallback()
    {
        QString iniPath = m_tempDir->filePath("test_readonly.ini");
        {
            QFile file(iniPath);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
            file.write("[Shortcuts]\nDataTableViewer.find=Ctrl+F\n");
            file.close();
            QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::ReadUser));
        }

        QWidget widget;
        dtv::ui::ActionRegistry registry(&widget);

        QSettings settings(iniPath, QSettings::IniFormat);
        registry.loadShortcuts(settings);

        QAction *findAct = registry.registerAction("DataTableViewer.find", "Find", QKeySequence::Find);
        QAction *copyAct = registry.registerAction("DataTableViewer.copy", "Copy", QKeySequence::Copy);

        QCOMPARE(findAct->shortcut(), QKeySequence(QKeySequence::Find));
        QCOMPARE(copyAct->shortcut(), QKeySequence(QKeySequence::Copy));

        // Attempting to save missing defaults to read-only settings must not crash
        // or mutate in-memory shortcuts away from compiled defaults.
        registry.saveDefaultsIfMissing(settings);
        settings.sync();

        QCOMPARE(registry.shortcut("DataTableViewer.find"), QKeySequence(QKeySequence::Find));
        QCOMPARE(registry.shortcut("DataTableViewer.copy"), QKeySequence(QKeySequence::Copy));

        // Restore write permissions so cleanup succeeds
        QFile::setPermissions(iniPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                      QFileDevice::ReadUser | QFileDevice::WriteUser);
    }

    void testWidgetDestroyedBeforeRegistryDoesNotCrash()
    {
        auto *widget = new QWidget();
        auto registry = std::make_unique<dtv::ui::ActionRegistry>(widget);
        QAction *act = registry->registerAction("DataTableViewer.test", "Test", QKeySequence("Ctrl+T"));
        Q_UNUSED(act);
        delete widget;
        // Destroying registry after widget was deleted must be safe and not double-free
        registry.reset();
    }
};

QTEST_MAIN(TestActionRegistry)
#include "test_action_registry.moc"
