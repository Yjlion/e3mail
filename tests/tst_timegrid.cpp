// SPDX-License-Identifier: MPL-2.0
// The calendar's week and day (ADR 0025, amendment 2026-11-10): the real
// TimeGrid.qml drawn offscreen, then clicked and dragged with the mouse and
// with a finger. What is tested is what the grid says the person did; what
// a move does to an event is tst_calendar's.
#include <QGuiApplication>
#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QSignalSpy>
#include <QtTest>

class TestTimeGrid : public QObject
{
    Q_OBJECT

    std::unique_ptr<QQuickView> m_view;
    QQuickItem *m_grid = nullptr;
    QQuickItem *m_canvas = nullptr;
    QQuickItem *m_hours = nullptr;
    QPointingDevice *m_finger = nullptr;

    static QVariantMap block(const char *uid, int startMin, int endMin, bool movable, int column = 0, int columns = 1)
    {
        return {{QStringLiteral("uid"), QString::fromLatin1(uid)},
                {QStringLiteral("title"), QString::fromLatin1(uid)},
                {QStringLiteral("timeText"), QStringLiteral("10:00 – 11:00")},
                {QStringLiteral("location"), QString()},
                {QStringLiteral("mine"), true},
                {QStringLiteral("myStatus"), QString()},
                {QStringLiteral("movable"), movable},
                {QStringLiteral("startMin"), startMin},
                {QStringLiteral("endMin"), endMin},
                {QStringLiteral("column"), column},
                {QStringLiteral("columns"), columns},
                {QStringLiteral("before"), false},
                {QStringLiteral("after"), false}};
    }

    // Three days: "mine" on the first, movable; "theirs" on the second, not;
    // a bar over the second and third.
    static QVariantMap sample()
    {
        QVariantList days;
        const char *isos[] = {"2026-11-16", "2026-11-17", "2026-11-18"};
        for (int i = 0; i < 3; ++i) {
            QVariantList blocks;
            if (i == 0)
                blocks.append(block("mine", 600, 660, true));
            if (i == 1)
                blocks.append(block("theirs", 600, 660, false));
            days.append(QVariantMap{{QStringLiteral("iso"), QString::fromLatin1(isos[i])},
                                    {QStringLiteral("weekday"), QStringLiteral("Day")},
                                    {QStringLiteral("number"), QString::number(16 + i)},
                                    {QStringLiteral("label"), QString::fromLatin1(isos[i])},
                                    {QStringLiteral("today"), false},
                                    {QStringLiteral("blocks"), blocks}});
        }
        QVariantMap bar = block("away", 0, 0, false);
        bar.insert(QStringLiteral("firstDay"), 1);
        bar.insert(QStringLiteral("days"), 2);
        bar.insert(QStringLiteral("lane"), 0);
        QVariantList hours;
        for (int h = 0; h < 24; ++h)
            hours.append(QStringLiteral("%1").arg(h, 2, 10, QLatin1Char('0')));
        return {{QStringLiteral("title"), QStringLiteral("16 – 18 November 2026")},
                {QStringLiteral("first"), QStringLiteral("2026-11-16")},
                {QStringLiteral("hours"), hours},
                {QStringLiteral("days"), days},
                {QStringLiteral("bars"), QVariantList{bar}},
                {QStringLiteral("lanes"), 1}};
    }

    void load(bool rightToLeft)
    {
        m_view = std::make_unique<QQuickView>();
        m_view->resize(700, 600);
        QQmlComponent c(m_view->engine());
        c.setData(QByteArray("import QtQuick\nimport E3mail\n"
                             "Item {\n"
                             "    LayoutMirroring.enabled: ")
                      + (rightToLeft ? "true" : "false")
                      + QByteArray("\n"
                                   "    LayoutMirroring.childrenInherit: true\n"
                                   "    TimeGrid { objectName: \"grid\"; anchors.fill: parent }\n"
                                   "}\n"),
                  QUrl());
        QVERIFY2(c.isReady(), qPrintable(c.errorString()));
        auto *root = qobject_cast<QQuickItem *>(c.create());
        QVERIFY(root);
        root->setParent(m_view.get());
        root->setParentItem(m_view->contentItem());
        root->setSize(QSizeF(700, 600));
        m_view->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_view.get()));
        m_grid = root->findChild<QQuickItem *>(QStringLiteral("grid"));
        QVERIFY(m_grid);
        m_grid->setProperty("grid", sample());
        m_canvas = m_grid->findChild<QQuickItem *>(QStringLiteral("canvas"));
        m_hours = m_grid->findChild<QQuickItem *>(QStringLiteral("hours"));
        QVERIFY(m_canvas && m_hours);
        // It opens at seven: ten o'clock is on the screen.
        QTRY_VERIFY(m_hours->property("contentY").toReal() > 300);
        QCOMPARE(m_grid->property("days").toList().size(), 3);
    }

    qreal dayX(int day) const
    {
        QVariant x;
        QMetaObject::invokeMethod(m_grid, "dayX", Q_RETURN_ARG(QVariant, x), Q_ARG(QVariant, day));
        return x.toReal();
    }
    // A point in the window: the middle of a day's column at a minute of it.
    QPoint at(int day, int minute, int dy = 0) const
    {
        const qreal x = dayX(day) + m_grid->property("dayWidth").toReal() / 2;
        const qreal y = minute / 60.0 * m_grid->property("hourHeight").toReal();
        return m_canvas->mapToScene(QPointF(x, y)).toPoint() + QPoint(0, dy);
    }
    // Press, travel in steps as a hand does, release.
    void drag(const QPoint &from, const QPoint &to)
    {
        QTest::mousePress(m_view.get(), Qt::LeftButton, {}, from);
        for (int i = 1; i <= 8; ++i)
            QTest::mouseMove(m_view.get(), from + (to - from) * i / 8);
        QTest::mouseRelease(m_view.get(), Qt::LeftButton, {}, to);
    }

private Q_SLOTS:
    void initTestCase() { m_finger = QTest::createTouchDevice(); }
    void init() { load(false); }
    void cleanup()
    {
        m_grid = m_canvas = m_hours = nullptr;
        m_view.reset();
    }

    void clicks()
    {
        QSignalSpy opened(m_grid, SIGNAL(eventClicked(QString)));
        QSignalSpy slot(m_grid, SIGNAL(slotClicked(QString, int)));
        QSignalSpy day(m_grid, SIGNAL(dayClicked(QString)));
        QSignalSpy moved(m_grid, SIGNAL(eventMoved(QVariant, QString, int)));

        // An event, movable or not, opens.
        QTest::mouseClick(m_view.get(), Qt::LeftButton, {}, at(0, 630));
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened[0][0].toString(), QStringLiteral("mine"));
        QTest::mouseClick(m_view.get(), Qt::LeftButton, {}, at(1, 630));
        QCOMPARE(opened.size(), 2);
        QCOMPARE(opened[1][0].toString(), QStringLiteral("theirs"));
        QVERIFY(slot.isEmpty());

        // An empty place: a new event at the half hour it is in.
        QTest::mouseClick(m_view.get(), Qt::LeftButton, {}, at(2, 13 * 60 + 20));
        QCOMPARE(slot.size(), 1);
        QCOMPARE(slot[0][0].toString(), QStringLiteral("2026-11-18"));
        QCOMPARE(slot[0][1].toInt(), 13 * 60);
        QTest::mouseClick(m_view.get(), Qt::LeftButton, {}, at(0, 13 * 60 + 40));
        QCOMPARE(slot[1][0].toString(), QStringLiteral("2026-11-16"));
        QCOMPARE(slot[1][1].toInt(), 13 * 60 + 30);

        // A day's name opens that day; the bar above the hours opens its event.
        const qreal mid = m_grid->property("dayWidth").toReal() / 2;
        QTest::mouseClick(m_view.get(), Qt::LeftButton, {}, m_grid->mapToScene(QPointF(dayX(1) + mid, 24)).toPoint());
        QCOMPARE(day.size(), 1);
        QCOMPARE(day[0][0].toString(), QStringLiteral("2026-11-17"));
        QTest::mouseClick(m_view.get(), Qt::LeftButton, {}, m_grid->mapToScene(QPointF(dayX(2) + mid, 48 + 12)).toPoint());
        QCOMPARE(opened.size(), 3);
        QCOMPARE(opened[2][0].toString(), QStringLiteral("away"));
        QVERIFY(moved.isEmpty());
    }

    void dragToAnotherTime()
    {
        QSignalSpy opened(m_grid, SIGNAL(eventClicked(QString)));
        QSignalSpy moved(m_grid, SIGNAL(eventMoved(QVariant, QString, int)));
        const qreal before = m_hours->property("contentY").toReal();
        // Held half an hour into it and taken an hour and a half down.
        drag(at(0, 630), at(0, 720));
        QCOMPARE(moved.size(), 1);
        QCOMPARE(moved[0][0].toMap().value(QStringLiteral("uid")).toString(), QStringLiteral("mine"));
        QCOMPARE(moved[0][1].toString(), QStringLiteral("2026-11-16"));
        QCOMPARE(moved[0][2].toInt(), 690);
        // The hours stayed where they were, and it was not a click.
        QCOMPARE(m_hours->property("contentY").toReal(), before);
        QVERIFY(opened.isEmpty());
        QVERIFY(!m_grid->property("drag").toMap().contains(QStringLiteral("ev")));

        // Twenty minutes down lands on the quarter hour.
        drag(at(0, 630), at(0, 650));
        QCOMPARE(moved.size(), 2);
        QCOMPARE(moved[1][2].toInt(), 615);
        // Held half an hour in and taken to the top of the day: it starts
        // at midnight, not half an hour before it.
        m_hours->setProperty("contentY", 0);
        drag(at(0, 630), at(0, 0));
        QCOMPARE(moved.size(), 3);
        QCOMPARE(moved[2][2].toInt(), 0);
    }

    void dragToAnotherDay()
    {
        QSignalSpy moved(m_grid, SIGNAL(eventMoved(QVariant, QString, int)));
        drag(at(0, 630), at(2, 630));
        QCOMPARE(moved.size(), 1);
        QCOMPARE(moved[0][1].toString(), QStringLiteral("2026-11-18"));
        QCOMPARE(moved[0][2].toInt(), 600);
        // Past the last day's far edge is still the last day.
        drag(at(0, 630), QPoint(m_view->width() - 1, at(2, 645).y()));
        QCOMPARE(moved.size(), 2);
        QCOMPARE(moved[1][1].toString(), QStringLiteral("2026-11-18"));
        QCOMPARE(moved[1][2].toInt(), 615);
    }

    // Taken up and put back where it was: nothing happened, not even a click.
    void putBack()
    {
        QSignalSpy opened(m_grid, SIGNAL(eventClicked(QString)));
        QSignalSpy moved(m_grid, SIGNAL(eventMoved(QVariant, QString, int)));
        QTest::mousePress(m_view.get(), Qt::LeftButton, {}, at(0, 630));
        QTest::mouseMove(m_view.get(), at(0, 660));
        QTest::mouseMove(m_view.get(), at(0, 700));
        // While it is held, the grid shows where it would land.
        QCOMPARE(m_grid->property("drag").toMap().value(QStringLiteral("startMin")).toInt(), 675);
        QTest::mouseMove(m_view.get(), at(0, 640));
        QTest::mouseMove(m_view.get(), at(0, 630));
        QTest::mouseRelease(m_view.get(), Qt::LeftButton, {}, at(0, 630));
        QVERIFY(moved.isEmpty());
        QVERIFY(opened.isEmpty());
    }

    // Someone else's meeting, or one of a series, stays where it is: the
    // drag scrolls the hours, as it does on an empty place.
    void whatMayNotMoveDoesNot()
    {
        QSignalSpy opened(m_grid, SIGNAL(eventClicked(QString)));
        QSignalSpy moved(m_grid, SIGNAL(eventMoved(QVariant, QString, int)));
        QSignalSpy stretched(m_grid, SIGNAL(eventStretched(QVariant, QString, int)));
        const qreal before = m_hours->property("contentY").toReal();
        drag(at(1, 630), at(1, 720));
        QVERIFY(moved.isEmpty());
        QVERIFY(opened.isEmpty());
        QVERIFY(m_hours->property("contentY").toReal() < before);
        // Nor has it an edge to pull.
        QTRY_VERIFY(!m_hours->property("moving").toBool());
        drag(at(1, 660, -3), at(1, 720));
        QVERIFY(stretched.isEmpty());
        QVERIFY(moved.isEmpty());
    }

    void dragTheLowerEdge()
    {
        QSignalSpy opened(m_grid, SIGNAL(eventClicked(QString)));
        QSignalSpy moved(m_grid, SIGNAL(eventMoved(QVariant, QString, int)));
        QSignalSpy stretched(m_grid, SIGNAL(eventStretched(QVariant, QString, int)));
        drag(at(0, 660, -3), at(0, 705));
        QCOMPARE(stretched.size(), 1);
        QCOMPARE(stretched[0][0].toMap().value(QStringLiteral("uid")).toString(), QStringLiteral("mine"));
        QCOMPARE(stretched[0][1].toString(), QStringLiteral("2026-11-16"));
        QCOMPARE(stretched[0][2].toInt(), 705);
        // Pulled above its start, it keeps a quarter of an hour.
        drag(at(0, 660, -3), at(0, 500));
        QCOMPARE(stretched.size(), 2);
        QCOMPARE(stretched[1][2].toInt(), 615);
        // Sideways changes nothing: the edge is this day's.
        drag(at(0, 660, -3), at(2, 660, -3));
        QCOMPARE(stretched.size(), 2);
        QVERIFY(moved.isEmpty());
        QVERIFY(opened.isEmpty());
    }

    // A finger scrolls; it never moves an event. A tap still opens it.
    void aFingerScrolls()
    {
        QSignalSpy opened(m_grid, SIGNAL(eventClicked(QString)));
        QSignalSpy moved(m_grid, SIGNAL(eventMoved(QVariant, QString, int)));
        QSignalSpy stretched(m_grid, SIGNAL(eventStretched(QVariant, QString, int)));
        const qreal before = m_hours->property("contentY").toReal();
        const QPoint from = at(0, 630), to = at(0, 720);
        QTest::touchEvent(m_view.get(), m_finger).press(0, from);
        for (int i = 1; i <= 8; ++i) {
            QTest::qWait(10); // a flick is measured in time as well as distance
            QTest::touchEvent(m_view.get(), m_finger).move(0, from + (to - from) * i / 8);
        }
        QTest::touchEvent(m_view.get(), m_finger).release(0, to);
        QVERIFY(moved.isEmpty());
        QVERIFY(opened.isEmpty());
        QVERIFY(m_hours->property("contentY").toReal() < before);
        // The same on its lower edge.
        QTRY_VERIFY(!m_hours->property("moving").toBool());
        const QPoint edge = at(0, 660, -3);
        QTest::touchEvent(m_view.get(), m_finger).press(0, edge);
        for (int i = 1; i <= 8; ++i) {
            QTest::qWait(10);
            QTest::touchEvent(m_view.get(), m_finger).move(0, edge + QPoint(0, 6 * i));
        }
        QTest::touchEvent(m_view.get(), m_finger).release(0, edge + QPoint(0, 48));
        QVERIFY(stretched.isEmpty());
        QVERIFY(moved.isEmpty());

        QTRY_VERIFY(!m_hours->property("moving").toBool());
        const QPoint tap = at(0, 630);
        QTest::touchEvent(m_view.get(), m_finger).press(0, tap);
        QTest::touchEvent(m_view.get(), m_finger).release(0, tap);
        QTRY_COMPARE(opened.size(), 1);
        QCOMPARE(opened[0][0].toString(), QStringLiteral("mine"));
    }

    // Right to left the days run the other way, and so does a drag.
    void rightToLeft()
    {
        cleanup();
        load(true);
        QVERIFY(dayX(0) > dayX(1));
        QVERIFY(dayX(1) > dayX(2));
        // The hours' margin is on the right.
        QVERIFY(dayX(0) + m_grid->property("dayWidth").toReal() < m_grid->width() - 10);
        QCOMPARE(dayX(2), 0.0);
        QSignalSpy opened(m_grid, SIGNAL(eventClicked(QString)));
        QSignalSpy moved(m_grid, SIGNAL(eventMoved(QVariant, QString, int)));
        QSignalSpy slot(m_grid, SIGNAL(slotClicked(QString, int)));
        QTest::mouseClick(m_view.get(), Qt::LeftButton, {}, at(0, 630));
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened[0][0].toString(), QStringLiteral("mine"));
        // To the left is later.
        QVERIFY(at(2, 630).x() < at(0, 630).x());
        drag(at(0, 630), at(2, 660));
        QCOMPARE(moved.size(), 1);
        QCOMPARE(moved[0][1].toString(), QStringLiteral("2026-11-18"));
        QCOMPARE(moved[0][2].toInt(), 630);
        QTest::mouseClick(m_view.get(), Qt::LeftButton, {}, at(2, 14 * 60 + 10));
        QCOMPARE(slot.size(), 1);
        QCOMPARE(slot[0][0].toString(), QStringLiteral("2026-11-18"));
    }
};

QTEST_MAIN(TestTimeGrid)
#include "tst_timegrid.moc"
