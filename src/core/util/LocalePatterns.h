// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QDate>
#include <QLocale>
#include <QString>

namespace e3 {

// Date patterns that QLocale has no format for, from CLDR through ICU, as Qt
// format strings. "6 Oct" in English is "10月6日" in Japanese and "6. Okt."
// in German; a fixed "d MMM" is wrong in most of the world.
class LocalePatterns
{
public:
    // Day and abbreviated month, without the year.
    static QString monthDay(const QLocale &locale);
    // The hour alone, as a Qt time format: "9 AM", "09", "9時".
    static QString hour(const QLocale &locale);
    // A span of days as the locale writes one: "9 – 15 November 2026",
    // "November 9 – 15, 2026", "2026年11月9日～15日". The parts the two days
    // share are written once, and one day alone is that day. `brief`
    // abbreviates the month and, for one day, adds its weekday: "Tue, 17
    // Nov 2026".
    static QString dateRange(const QLocale &locale, const QDate &first, const QDate &last, bool brief = false);
};

} // namespace e3
