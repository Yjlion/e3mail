// SPDX-License-Identifier: MPL-2.0
#include "CalendarLayout.h"

#include <algorithm>

namespace e3::mail {

namespace {

constexpr qint64 kDayMs = 24LL * 3600 * 1000;

// The extent a block is drawn with: never shorter than kMinDrawn, and never
// past midnight, so a late short event is moved up rather than cut off.
int drawnStart(const CalendarLayout::Block &b)
{
    return qMin(b.startMin, CalendarLayout::kDayMinutes - CalendarLayout::kMinDrawn);
}

int drawnEnd(const CalendarLayout::Block &b)
{
    return qMax(b.endMin, drawnStart(b) + CalendarLayout::kMinDrawn);
}

// Blocks that overlap, directly or through others, share a column count;
// each takes the first column that is free when it starts.
void pack(QList<CalendarLayout::Block> &blocks)
{
    std::stable_sort(blocks.begin(), blocks.end(), [](const CalendarLayout::Block &a, const CalendarLayout::Block &b) {
        if (a.startMin != b.startMin)
            return a.startMin < b.startMin;
        return a.endMin > b.endMin;
    });
    QList<int> columnEnds; // per column of the current group: where its last block ends
    qsizetype groupFirst = 0;
    int groupEnd = 0;
    const auto close = [&](qsizetype upTo) {
        for (qsizetype i = groupFirst; i < upTo; ++i)
            blocks[i].columns = int(columnEnds.size());
    };
    for (qsizetype i = 0; i < blocks.size(); ++i) {
        CalendarLayout::Block &b = blocks[i];
        const int s = drawnStart(b);
        if (!columnEnds.isEmpty() && s >= groupEnd) {
            close(i);
            columnEnds.clear();
            groupFirst = i;
        }
        int column = -1;
        for (int c = 0; c < columnEnds.size() && column < 0; ++c) {
            if (columnEnds[c] <= s)
                column = c;
        }
        if (column < 0) {
            column = int(columnEnds.size());
            columnEnds.append(0);
        }
        columnEnds[column] = drawnEnd(b);
        b.column = column;
        groupEnd = qMax(groupEnd, drawnEnd(b));
    }
    close(blocks.size());
}

int minuteOf(const QTime &t, bool roundUp)
{
    const int ms = t.msecsSinceStartOfDay();
    return roundUp ? (ms + 59999) / 60000 : ms / 60000;
}

} // namespace

CalendarLayout::Result CalendarLayout::layout(const QList<Item> &items, const QDate &first, int dayCount,
                                              const QTimeZone &zone)
{
    Result r;
    if (!first.isValid() || dayCount <= 0)
        return r;
    r.days.reserve(dayCount);
    for (int i = 0; i < dayCount; ++i)
        r.days.append(Day{first.addDays(i), {}});
    const QDate last = first.addDays(dayCount - 1);

    for (int index = 0; index < items.size(); ++index) {
        const Item &it = items[index];
        if (!it.start.isValid())
            continue;
        const QDateTime end = it.end.isValid() && it.end > it.start ? it.end : it.start;
        const bool whole = it.allDay || it.start.msecsTo(end) >= kDayMs;
        // The first and last day it touches. An end at midnight belongs to
        // the day before.
        QDate from, to;
        QDateTime s, t;
        if (it.allDay) {
            from = it.start.toUTC().date();
            to = end > it.start ? end.toUTC().date().addDays(-1) : from;
        } else {
            s = it.start.toTimeZone(zone);
            t = end.toTimeZone(zone);
            from = s.date();
            to = t > s && t.time() == QTime(0, 0) ? t.date().addDays(-1) : t.date();
        }
        if (to < from)
            to = from;
        if (to < first || from > last)
            continue;

        if (whole) {
            Bar bar;
            bar.item = index;
            bar.before = from < first;
            bar.after = to > last;
            bar.firstDay = int(first.daysTo(qMax(from, first)));
            bar.days = int(qMax(from, first).daysTo(qMin(to, last))) + 1;
            r.bars.append(bar);
            continue;
        }

        for (QDate d = qMax(from, first); d <= qMin(to, last); d = d.addDays(1)) {
            Block b;
            b.item = index;
            b.before = d > from;
            b.after = d < to;
            b.startMin = b.before ? 0 : minuteOf(s.time(), false);
            b.endMin = (b.after || t.date() > d) ? kDayMinutes : minuteOf(t.time(), true);
            // The hour the clocks repeat: an event may end at an earlier
            // clock time than it began. It is drawn as long as it lasts.
            if (!b.before && !b.after && b.endMin < b.startMin)
                b.endMin = b.startMin + int(qMin<qint64>(kDayMinutes, (it.start.msecsTo(end) + 59999) / 60000));
            b.endMin = qBound(b.startMin, b.endMin, kDayMinutes);
            r.days[first.daysTo(d)].blocks.append(b);
        }
    }

    for (Day &day : r.days)
        pack(day.blocks);

    // Bars: by first day, the longer first, each in the first lane free there.
    std::stable_sort(r.bars.begin(), r.bars.end(), [](const Bar &a, const Bar &b) {
        if (a.firstDay != b.firstDay)
            return a.firstDay < b.firstDay;
        return a.days > b.days;
    });
    QList<int> laneEnds; // per lane: the first day free again
    for (Bar &bar : r.bars) {
        int lane = -1;
        for (int l = 0; l < laneEnds.size() && lane < 0; ++l) {
            if (laneEnds[l] <= bar.firstDay)
                lane = l;
        }
        if (lane < 0) {
            lane = int(laneEnds.size());
            laneEnds.append(0);
        }
        laneEnds[lane] = bar.firstDay + bar.days;
        bar.lane = lane;
    }
    r.lanes = int(laneEnds.size());
    return r;
}

} // namespace e3::mail
