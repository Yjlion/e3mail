// SPDX-License-Identifier: MPL-2.0
#pragma once

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
};

} // namespace e3
