#include <QtTest>
#include <QSignalSpy>
#include <QPushButton>
#include "ui/page_bar.h"

using namespace dtv::ui;

class TestPageBar : public QObject {
    Q_OBJECT
private slots:
    void testSmallTableHidesPager() {
        PageBar bar;
        dtv::core::PagerState state;
        state.page = 1;
        state.pageSize = 500;
        state.total = 50;
        state.hasMore = false;
        bar.setPagerState(state);
        QCOMPARE(bar.shouldBeVisible(), false);
    }

    void testPendingCountDisplayAndEnabled() {
        PageBar bar;
        dtv::core::PagerState state;
        state.page = 1;
        state.pageSize = 500;
        state.total = std::nullopt;
        state.hasMore = true;
        bar.setPagerState(state);
        QCOMPARE(bar.shouldBeVisible(), true);
        QCOMPARE(state.canFirst(), false);
        QCOMPARE(state.canPrev(), false);
        QCOMPARE(state.canNext(), true);
        QCOMPARE(state.canLast(), false);
    }

    void testKnownTotalNavigationStates() {
        PageBar bar;
        dtv::core::PagerState state;
        state.pageSize = 500;
        state.total = 1500;

        // Page 1
        state.page = 1;
        bar.setPagerState(state);
        QCOMPARE(state.canFirst(), false);
        QCOMPARE(state.canPrev(), false);
        QCOMPARE(state.canNext(), true);
        QCOMPARE(state.canLast(), true);

        // Page 2
        state.page = 2;
        bar.setPagerState(state);
        QCOMPARE(state.canFirst(), true);
        QCOMPARE(state.canPrev(), true);
        QCOMPARE(state.canNext(), true);
        QCOMPARE(state.canLast(), true);

        // Page 3 (Last page)
        state.page = 3;
        bar.setPagerState(state);
        QCOMPARE(state.canFirst(), true);
        QCOMPARE(state.canPrev(), true);
        QCOMPARE(state.canNext(), false);
        QCOMPARE(state.canLast(), false);
    }

    void testButtonSignals() {
        PageBar bar;
        QSignalSpy spyFirst(&bar, &PageBar::firstClicked);
        QSignalSpy spyPrev(&bar, &PageBar::prevClicked);
        QSignalSpy spyNext(&bar, &PageBar::nextClicked);
        QSignalSpy spyLast(&bar, &PageBar::lastClicked);

        dtv::core::PagerState state;
        state.pageSize = 500;
        state.total = 1500;
        state.page = 2;
        bar.setPagerState(state);

        // Click the real buttons so the test covers the PageBar wiring, not just
        // QSignalSpy. Middle page keeps all four buttons enabled.
        const QList<QPushButton *> buttons = bar.findChildren<QPushButton *>();
        QCOMPARE(buttons.size(), 4);
        for(QPushButton *button : buttons) {
            QVERIFY(button->isEnabled());
        }

        buttons.at(0)->click(); // first
        buttons.at(1)->click(); // prev
        buttons.at(2)->click(); // next
        buttons.at(3)->click(); // last

        QCOMPARE(spyFirst.count(), 1);
        QCOMPARE(spyPrev.count(), 1);
        QCOMPARE(spyNext.count(), 1);
        QCOMPARE(spyLast.count(), 1);
    }
};

QTEST_MAIN(TestPageBar)
#include "test_page_bar.moc"
