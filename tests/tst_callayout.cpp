// SPDX-License-Identifier: MPL-2.0
#include "mail/CalendarLayout.h"
#include "util/LocalePatterns.h"

#include <QtTest>

using namespace e3;
using mail::CalendarLayout;

namespace {

const QTimeZone &berlin()
{
    static const QTimeZone z("Europe/Berlin");
    return z;
}

// A timed item on the wall clock of Berlin.
CalendarLayout::Item at(const QDate &day, const QTime &from, const QDate &endDay, const QTime &to)
{
    return {QDateTime(day, from, berlin()).toUTC(), QDateTime(endDay, to, berlin()).toUTC(), false};
}

CalendarLayout::Item at(const QDate &day, int h1, int m1, int h2, int m2)
{
    return at(day, QTime(h1, m1), day, QTime(h2, m2));
}

// An all-day item from `first` to `last`, both included.
CalendarLayout::Item allDay(const QDate &first, const QDate &last)
{
    return {QDateTime(first, QTime(0, 0), QTimeZone::UTC), QDateTime(last.addDays(1), QTime(0, 0), QTimeZone::UTC), true};
}

// The block of an item on a day, or one with item -1.
CalendarLayout::Block blockOf(const CalendarLayout::Result &r, int day, int item)
{
    for (const CalendarLayout::Block &b : r.days[day].blocks) {
        if (b.item == item)
            return b;
    }
    CalendarLayout::Block none;
    none.item = -1;
    return none;
}

CalendarLayout::Bar barOf(const CalendarLayout::Result &r, int item)
{
    for (const CalendarLayout::Bar &b : r.bars) {
        if (b.item == item)
            return b;
    }
    CalendarLayout::Bar none;
    none.item = -1;
    return none;
}

const QDate monday(2026, 11, 9); // a week without a clock change

} // namespace

class TestCalendarLayout : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { QVERIFY2(berlin().isValid(), "this machine has no Europe/Berlin zone"); }

    void aDayOfItsOwn()
    {
        const auto r = CalendarLayout::layout({at(monday.addDays(1), 10, 0, 11, 30)}, monday, 7, berlin());
        QCOMPARE(r.days.size(), 7);
        QCOMPARE(r.days[0].date, monday);
        QCOMPARE(r.days[6].date, monday.addDays(6));
        QVERIFY(r.days[0].blocks.isEmpty());
        QCOMPARE(r.days[1].blocks.size(), 1);
        const auto b = r.days[1].blocks[0];
        QCOMPARE(b.startMin, 600);
        QCOMPARE(b.endMin, 690);
        QCOMPARE(b.column, 0);
        QCOMPARE(b.columns, 1);
        QVERIFY(!b.before && !b.after);
        QVERIFY(r.bars.isEmpty());
        QCOMPARE(r.lanes, 0);
    }

    // The clock that places an event is the reader's, not the event's.
    void placedInTheReadersZone()
    {
        const CalendarLayout::Item noonUtc{QDateTime(monday, QTime(12, 0), QTimeZone::UTC),
                                           QDateTime(monday, QTime(13, 0), QTimeZone::UTC), false};
        QCOMPARE(CalendarLayout::layout({noonUtc}, monday, 1, berlin()).days[0].blocks[0].startMin, 13 * 60);
        const QTimeZone tokyo("Asia/Tokyo");
        QCOMPARE(CalendarLayout::layout({noonUtc}, monday, 1, tokyo).days[0].blocks[0].startMin, 21 * 60);
        // 20:00 in New York on Monday is Tuesday in Berlin.
        const QTimeZone newYork("America/New_York");
        const CalendarLayout::Item late{QDateTime(monday, QTime(20, 0), newYork).toUTC(),
                                        QDateTime(monday, QTime(21, 0), newYork).toUTC(), false};
        const auto r = CalendarLayout::layout({late}, monday, 7, berlin());
        QVERIFY(r.days[0].blocks.isEmpty());
        QCOMPARE(r.days[1].blocks.size(), 1);
        QCOMPARE(r.days[1].blocks[0].startMin, 2 * 60);
    }

    void overlapsShareTheWidth()
    {
        // Two at once: side by side. A third, later, has the day to itself.
        const auto r = CalendarLayout::layout({at(monday, 10, 0, 11, 0), at(monday, 10, 30, 11, 30), at(monday, 14, 0, 15, 0)},
                                              monday, 1, berlin());
        const auto a = blockOf(r, 0, 0), b = blockOf(r, 0, 1), c = blockOf(r, 0, 2);
        QCOMPARE(a.column, 0);
        QCOMPARE(b.column, 1);
        QCOMPARE(a.columns, 2);
        QCOMPARE(b.columns, 2);
        QCOMPARE(c.column, 0);
        QCOMPARE(c.columns, 1);
    }

    // A overlaps B, B overlaps C, A and C do not: two columns are enough,
    // and all three are as wide as each other.
    void aChainNeedsTwoColumns()
    {
        const auto r = CalendarLayout::layout({at(monday, 9, 0, 10, 30), at(monday, 10, 0, 11, 30), at(monday, 11, 0, 12, 0)},
                                              monday, 1, berlin());
        const auto a = blockOf(r, 0, 0), b = blockOf(r, 0, 1), c = blockOf(r, 0, 2);
        QCOMPARE(a.column, 0);
        QCOMPARE(b.column, 1);
        QCOMPARE(c.column, 0);
        QCOMPARE(a.columns, 2);
        QCOMPARE(b.columns, 2);
        QCOMPARE(c.columns, 2);
    }

    void threeAtOnce()
    {
        const auto r = CalendarLayout::layout({at(monday, 9, 0, 12, 0), at(monday, 9, 0, 10, 0), at(monday, 9, 30, 10, 30),
                                               at(monday, 10, 0, 11, 0)},
                                              monday, 1, berlin());
        // The longer of two that start together comes first.
        QCOMPARE(r.days[0].blocks[0].item, 0);
        QCOMPARE(blockOf(r, 0, 0).column, 0);
        QCOMPARE(blockOf(r, 0, 1).column, 1);
        QCOMPARE(blockOf(r, 0, 2).column, 2);
        // The fourth starts as the second ends, and takes its column.
        QCOMPARE(blockOf(r, 0, 3).column, 1);
        for (int i = 0; i < 4; ++i)
            QCOMPARE(blockOf(r, 0, i).columns, 3);
    }

    // Back to back is not an overlap.
    void touchingIsNotOverlapping()
    {
        const auto r = CalendarLayout::layout({at(monday, 9, 0, 10, 0), at(monday, 10, 0, 11, 0)}, monday, 1, berlin());
        QCOMPARE(blockOf(r, 0, 0).columns, 1);
        QCOMPARE(blockOf(r, 0, 1).columns, 1);
        QCOMPARE(blockOf(r, 0, 1).column, 0);
    }

    // A block is never drawn shorter than kMinDrawn, so two short events ten
    // minutes apart would be drawn over each other: they get a column each.
    void shortEventsDoNotCoverEachOther()
    {
        const auto r = CalendarLayout::layout({at(monday, 9, 0, 9, 5), at(monday, 9, 10, 9, 15), at(monday, 9, 20, 9, 25)},
                                              monday, 1, berlin());
        QCOMPARE(blockOf(r, 0, 0).column, 0);
        QCOMPARE(blockOf(r, 0, 1).column, 1);
        QCOMPARE(blockOf(r, 0, 0).columns, 2);
        // Twenty minutes after the first began, its column is free again.
        QCOMPARE(blockOf(r, 0, 2).column, 0);
        QCOMPARE(blockOf(r, 0, 2).columns, 2);
        // What is reported is still the event's own time.
        QCOMPARE(blockOf(r, 0, 0).startMin, 540);
        QCOMPARE(blockOf(r, 0, 0).endMin, 545);
    }

    void zeroLength()
    {
        const auto r = CalendarLayout::layout({at(monday, 12, 0, 12, 0), at(monday, 12, 10, 13, 0)}, monday, 1, berlin());
        const auto a = blockOf(r, 0, 0);
        QCOMPARE(a.startMin, 720);
        QCOMPARE(a.endMin, 720);
        QCOMPARE(a.columns, 2);
        // An end before the start is read as no length, not as a negative one.
        CalendarLayout::Item backwards = at(monday, 12, 0, 11, 0);
        const auto r2 = CalendarLayout::layout({backwards}, monday, 1, berlin());
        QCOMPARE(r2.days[0].blocks.size(), 1);
        QCOMPARE(r2.days[0].blocks[0].startMin, 720);
        QCOMPARE(r2.days[0].blocks[0].endMin, 720);
    }

    void acrossMidnight()
    {
        const auto r = CalendarLayout::layout({at(monday, QTime(22, 0), monday.addDays(1), QTime(1, 30))}, monday, 7, berlin());
        QCOMPARE(r.days[0].blocks.size(), 1);
        QCOMPARE(r.days[1].blocks.size(), 1);
        const auto evening = r.days[0].blocks[0], night = r.days[1].blocks[0];
        QCOMPARE(evening.startMin, 22 * 60);
        QCOMPARE(evening.endMin, 1440);
        QVERIFY(!evening.before);
        QVERIFY(evening.after);
        QCOMPARE(night.startMin, 0);
        QCOMPARE(night.endMin, 90);
        QVERIFY(night.before);
        QVERIFY(!night.after);
        QVERIFY(r.bars.isEmpty());
    }

    // Ending at midnight is ending that day: nothing on the next.
    void untilMidnight()
    {
        const auto r = CalendarLayout::layout({at(monday, QTime(21, 0), monday.addDays(1), QTime(0, 0))}, monday, 7, berlin());
        QCOMPARE(r.days[0].blocks.size(), 1);
        QCOMPARE(r.days[0].blocks[0].endMin, 1440);
        QVERIFY(!r.days[0].blocks[0].after);
        QVERIFY(r.days[1].blocks.isEmpty());
    }

    // A late, short event still fits in the day it is on.
    void lateAndShort()
    {
        const auto r = CalendarLayout::layout({at(monday, 23, 50, 23, 55), at(monday, 23, 30, 23, 45)}, monday, 1, berlin());
        // Drawn from 23:40 to midnight, it overlaps the one drawn from 23:30 to 23:50.
        QCOMPARE(blockOf(r, 0, 0).columns, 2);
        QCOMPARE(blockOf(r, 0, 0).startMin, 23 * 60 + 50);
    }

    void clippedToTheDaysShown()
    {
        // Sunday night into Monday; Sunday next week into the Monday after;
        // and one wholly outside.
        const QList<CalendarLayout::Item> items{
            at(monday.addDays(-1), QTime(23, 0), monday, QTime(2, 0)),
            at(monday.addDays(6), QTime(23, 0), monday.addDays(7), QTime(2, 0)),
            at(monday.addDays(8), 9, 0, 10, 0),
            at(monday.addDays(-2), 9, 0, 10, 0),
        };
        const auto r = CalendarLayout::layout(items, monday, 7, berlin());
        QCOMPARE(r.days[0].blocks.size(), 1);
        QVERIFY(r.days[0].blocks[0].before);
        QCOMPARE(r.days[0].blocks[0].startMin, 0);
        QCOMPARE(r.days[0].blocks[0].endMin, 120);
        QCOMPARE(r.days[6].blocks.size(), 1);
        QVERIFY(r.days[6].blocks[0].after);
        QCOMPARE(r.days[6].blocks[0].endMin, 1440);
        int total = 0;
        for (const auto &d : r.days)
            total += int(d.blocks.size());
        QCOMPARE(total, 2);
    }

    void allDayIsABar()
    {
        // One day; three days; and one that began last week and ends next.
        const auto r = CalendarLayout::layout({allDay(monday.addDays(2), monday.addDays(2)),
                                               allDay(monday.addDays(3), monday.addDays(5)),
                                               allDay(monday.addDays(-3), monday.addDays(9))},
                                              monday, 7, berlin());
        for (const auto &d : r.days)
            QVERIFY(d.blocks.isEmpty());
        QCOMPARE(r.bars.size(), 3);
        const auto one = barOf(r, 0), three = barOf(r, 1), long_ = barOf(r, 2);
        QCOMPARE(one.firstDay, 2);
        QCOMPARE(one.days, 1);
        QVERIFY(!one.before && !one.after);
        QCOMPARE(three.firstDay, 3);
        QCOMPARE(three.days, 3);
        QCOMPARE(long_.firstDay, 0);
        QCOMPARE(long_.days, 7);
        QVERIFY(long_.before);
        QVERIFY(long_.after);
        // The long one is first and takes the top lane; the other two do
        // not overlap each other and share the second.
        QCOMPARE(long_.lane, 0);
        QCOMPARE(one.lane, 1);
        QCOMPARE(three.lane, 1);
        QCOMPARE(r.lanes, 2);
    }

    // An all-day event's days are its dates, wherever the reader is: the
    // tenth is the tenth in Tokyo and in Los Angeles.
    void allDayKeepsItsDate()
    {
        const QList<CalendarLayout::Item> items{allDay(monday.addDays(1), monday.addDays(1))};
        for (const char *zone : {"Asia/Tokyo", "America/Los_Angeles", "Pacific/Kiritimati", "Europe/Berlin"}) {
            const auto r = CalendarLayout::layout(items, monday, 7, QTimeZone(zone));
            QCOMPARE(r.bars.size(), 1);
            QCOMPARE(r.bars[0].firstDay, 1);
            QCOMPARE(r.bars[0].days, 1);
        }
        // The day before and the day after do not show it.
        QVERIFY(CalendarLayout::layout(items, monday, 1, QTimeZone("America/Los_Angeles")).bars.isEmpty());
        QVERIFY(CalendarLayout::layout(items, monday.addDays(2), 1, QTimeZone("Asia/Tokyo")).bars.isEmpty());
    }

    void barsStackWhereTheyOverlap()
    {
        const auto r = CalendarLayout::layout({allDay(monday, monday.addDays(1)), allDay(monday.addDays(1), monday.addDays(2)),
                                               allDay(monday.addDays(2), monday.addDays(3)), allDay(monday, monday)},
                                              monday, 7, berlin());
        QCOMPARE(barOf(r, 0).lane, 0);
        QCOMPARE(barOf(r, 3).lane, 1); // the same first day, shorter: below
        QCOMPARE(barOf(r, 1).lane, 1); // Tuesday is taken in the first lane, free in the second
        QCOMPARE(barOf(r, 2).lane, 0); // Wednesday is free in the first again
        QCOMPARE(r.lanes, 2);
    }

    // A day long or longer is a bar, timed or not; a minute less is blocks.
    void aDayLongIsABar()
    {
        const auto r = CalendarLayout::layout({at(monday, QTime(9, 0), monday.addDays(1), QTime(9, 0)),
                                               at(monday.addDays(3), QTime(9, 0), monday.addDays(4), QTime(8, 59)),
                                               at(monday.addDays(1), QTime(18, 0), monday.addDays(3), QTime(0, 0))},
                                              monday, 7, berlin());
        QCOMPARE(r.bars.size(), 2);
        QCOMPARE(barOf(r, 0).firstDay, 0);
        QCOMPARE(barOf(r, 0).days, 2);
        // Ending at midnight, it does not reach into Thursday.
        QCOMPARE(barOf(r, 2).firstDay, 1);
        QCOMPARE(barOf(r, 2).days, 2);
        QCOMPARE(r.days[3].blocks.size(), 1);
        QCOMPARE(r.days[4].blocks.size(), 1);
        QCOMPARE(r.days[0].blocks.size(), 0);
    }

    // 29 March 2026 in Berlin has no 02:00 to 03:00. The day is still drawn
    // as 24 rows and an event sits at its clock time.
    void theDayTheClocksGoForward()
    {
        const QDate day(2026, 3, 29);
        const QDateTime oneThirty = QDateTime(day, QTime(1, 30), berlin());
        const CalendarLayout::Item across{oneThirty.toUTC(), oneThirty.toUTC().addSecs(3600), false}; // ends 03:30
        const auto r = CalendarLayout::layout({across, at(day, 9, 0, 10, 0), at(day.addDays(-1), 9, 0, 10, 0)}, day.addDays(-1), 2,
                                              berlin());
        QCOMPARE(blockOf(r, 1, 0).startMin, 90);
        QCOMPARE(blockOf(r, 1, 0).endMin, 210);
        QCOMPARE(blockOf(r, 1, 1).startMin, 540);
        QCOMPARE(blockOf(r, 0, 2).startMin, 540);
    }

    // 25 October 2026 has 02:00 to 03:00 twice. An event from the first
    // 02:30 to the second 02:15 ends at an earlier clock time than it began.
    void theDayTheClocksGoBack()
    {
        const QDate day(2026, 10, 25);
        const QDateTime first0230 = QDateTime(day, QTime(0, 30), berlin()).toUTC().addSecs(2 * 3600);
        QCOMPARE(first0230.toTimeZone(berlin()).time(), QTime(2, 30));
        const CalendarLayout::Item item{first0230, first0230.addSecs(45 * 60), false};
        QCOMPARE(item.end.toTimeZone(berlin()).time(), QTime(2, 15));
        const auto r = CalendarLayout::layout({item, at(day, 12, 0, 13, 0)}, day, 1, berlin());
        QCOMPARE(blockOf(r, 0, 0).startMin, 150);
        QCOMPARE(blockOf(r, 0, 0).endMin, 195); // as long as it lasts
        QCOMPARE(blockOf(r, 0, 1).startMin, 720);
        QCOMPARE(blockOf(r, 0, 1).endMin, 780);
    }

    void nothingToShow()
    {
        QVERIFY(CalendarLayout::layout({}, QDate(), 7, berlin()).days.isEmpty());
        QVERIFY(CalendarLayout::layout({}, monday, 0, berlin()).days.isEmpty());
        const auto r = CalendarLayout::layout({CalendarLayout::Item{}}, monday, 3, berlin());
        QCOMPARE(r.days.size(), 3);
        QVERIFY(r.bars.isEmpty());
    }

    // The gutter's hours and the week's title are the locale's (CLDR
    // through ICU), not a fixed pattern.
    void hoursFollowLocale()
    {
        const auto nine = [](const char *name, int h) {
            const QLocale l(QString::fromLatin1(name));
            return l.toString(QTime(h, 0), e3::LocalePatterns::hour(l));
        };
        const QString us = nine("en_US", 9);
        QVERIFY2(us.startsWith(u'9') && us.contains(QLatin1String("AM")) && !us.contains(u':'), qPrintable(us));
        QVERIFY2(nine("en_US", 15).startsWith(u'3') && nine("en_US", 15).contains(QLatin1String("PM")), qPrintable(nine("en_US", 15)));
        QCOMPARE(nine("en_GB", 9), QStringLiteral("09"));
        QCOMPARE(nine("de_DE", 15), QStringLiteral("15 Uhr"));
        QVERIFY2(nine("ja_JP", 9).contains(QStringLiteral("時")), qPrintable(nine("ja_JP", 9)));
    }

    void rangesFollowLocale()
    {
        const auto range = [](const char *name, const QDate &a, const QDate &b) {
            return e3::LocalePatterns::dateRange(QLocale(QString::fromLatin1(name)), a, b);
        };
        // Within a month, the month and the year are written once.
        const QString gb = range("en_GB", monday, monday.addDays(6));
        QVERIFY2(gb.startsWith(u'9') && gb.contains(QLatin1String("15")) && gb.endsWith(QLatin1String("November 2026")), qPrintable(gb));
        QCOMPARE(gb.count(QLatin1String("November")), 1);
        const QString us = range("en_US", monday, monday.addDays(6));
        QVERIFY2(us.startsWith(QLatin1String("November 9")) && us.endsWith(QLatin1String("2026")), qPrintable(us));
        QCOMPARE(us.count(QLatin1String("2026")), 1);
        // Across two months both are named; across two years, both years.
        const QString two = range("en_GB", QDate(2026, 11, 30), QDate(2026, 12, 6));
        QVERIFY2(two.contains(QLatin1String("November")) && two.contains(QLatin1String("December")), qPrintable(two));
        QCOMPARE(range("en_GB", QDate(2026, 12, 28), QDate(2027, 1, 3)).count(QLatin1String("202")), 2);
        const QString ja = range("ja_JP", monday, monday.addDays(6));
        QVERIFY2(ja.startsWith(QStringLiteral("2026年11月9日")), qPrintable(ja));
        const QString de = range("de_DE", monday, monday.addDays(6));
        QVERIFY2(de.startsWith(QLatin1String("9.")) && de.endsWith(QLatin1String("15. November 2026")), qPrintable(de));
        QVERIFY(range("en_GB", QDate(), monday).isEmpty());
        // One day is that day; briefly, with its weekday and a short month.
        const QString one = range("en_GB", monday, monday);
        QVERIFY2(one.contains(QLatin1String("9 November 2026")) && !one.contains(u'–'), qPrintable(one));
        const QString brief = e3::LocalePatterns::dateRange(QLocale(QStringLiteral("en_GB")), monday, monday, true);
        QVERIFY2(brief.startsWith(QLatin1String("Mon")) && brief.contains(QLatin1String("Nov")) && !brief.contains(QLatin1String("November")),
                 qPrintable(brief));
        const QString span = e3::LocalePatterns::dateRange(QLocale(QStringLiteral("en_GB")), monday, monday.addDays(2), true);
        QVERIFY2(span.startsWith(u'9') && span.endsWith(QLatin1String("11 Nov 2026")), qPrintable(span));
    }
};

QTEST_GUILESS_MAIN(TestCalendarLayout)
#include "tst_callayout.moc"
