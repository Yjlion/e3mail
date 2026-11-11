// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QTimeZone>

namespace e3::mail {

// Where events sit in a week or a day drawn as a grid of hours (ADR 0025,
// amendment 2026-11-10). It only places; it reads no store and knows no
// event beyond when it starts and ends, so the rules can be tested alone.
//
// A day is drawn as 24 rows of the wall clock in `zone`, also on the two
// days a year that last 23 or 25 hours: an event sits at the time its
// clock shows.
class CalendarLayout
{
public:
    // The shortest a block is drawn, in minutes, and so the least two blocks
    // must be apart to share a column.
    static constexpr int kMinDrawn = 20;
    static constexpr int kDayMinutes = 24 * 60;

    struct Item
    {
        QDateTime start;
        QDateTime end; // exclusive
        // All-day: the dates are those of `start` and `end` in UTC (ICal::Event).
        bool allDay = false;
    };

    // A timed event's part on one day.
    struct Block
    {
        int item = 0;      // index into the items given
        int startMin = 0;  // minutes of the wall clock from the day's midnight
        int endMin = 0;    // 1440 when it runs to midnight
        int column = 0;    // among the blocks it overlaps
        int columns = 1;   // how many columns that group needs
        bool before = false; // it began on an earlier day
        bool after = false;  // it goes on into the next day
    };

    // An all-day event, or one lasting a day or more: a bar above the hours.
    struct Bar
    {
        int item = 0;
        int firstDay = 0;  // index into the days shown
        int days = 1;      // how many of them it covers
        int lane = 0;      // its row in the band
        bool before = false; // it began before the first day shown
        bool after = false;  // it goes on after the last
    };

    struct Day
    {
        QDate date;
        QList<Block> blocks; // by start, then the longer first
    };

    struct Result
    {
        QList<Day> days;  // always `dayCount` of them
        QList<Bar> bars;  // by first day, then the longer first
        int lanes = 0;    // rows the band needs
    };

    static Result layout(const QList<Item> &items, const QDate &first, int dayCount, const QTimeZone &zone);
};

} // namespace e3::mail
