// SPDX-License-Identifier: MPL-2.0
#include "LocalePatterns.h"

#include <QHash>
#include <QMutex>

#include <QTimeZone>

#include <unicode/udateintervalformat.h>
#include <unicode/udatpg.h>

#include <memory>

namespace e3 {

namespace {

QString bestPattern(const QByteArray &localeName, const char16_t *skeleton)
{
    UErrorCode err = U_ZERO_ERROR;
    std::unique_ptr<UDateTimePatternGenerator, decltype(&udatpg_close)> gen(
        udatpg_open(localeName.constData(), &err), udatpg_close);
    if (U_FAILURE(err))
        return {};
    UChar buf[64];
    const int32_t n = udatpg_getBestPattern(gen.get(), reinterpret_cast<const UChar *>(skeleton), -1, buf, 64, &err);
    if (U_FAILURE(err))
        return {};
    // ICU and Qt patterns agree on d, M and quoting; Qt has no stand-alone
    // month (L), and its M is the same thing in a day-month pattern.
    QString p = QString(reinterpret_cast<const QChar *>(buf), n);
    p.replace(u'L', u'M');
    return p;
}

} // namespace

QString LocalePatterns::monthDay(const QLocale &locale)
{
    static QMutex mutex;
    static QHash<QString, QString> cache;
    const QMutexLocker lock(&mutex);
    auto it = cache.find(locale.name());
    if (it == cache.end()) {
        QString p = bestPattern(locale.name().toLatin1(), u"MMMd");
        it = cache.insert(locale.name(), p.isEmpty() ? QStringLiteral("d MMM") : p);
    }
    return *it;
}

QString LocalePatterns::hour(const QLocale &locale)
{
    static QMutex mutex;
    static QHash<QString, QString> cache;
    const QMutexLocker lock(&mutex);
    auto it = cache.find(locale.name());
    if (it == cache.end()) {
        // "j": the hour in the locale's own cycle, with its day period if it
        // has one. ICU's "a" is Qt's "Ap" (the locale's own case); quoted
        // text is left alone.
        const QString icu = bestPattern(locale.name().toLatin1(), u"j");
        QString p;
        bool quoted = false;
        for (const QChar c : icu) {
            if (c == u'\'')
                quoted = !quoted;
            if (!quoted && c == u'a')
                p += QStringLiteral("Ap");
            else
                p += c;
        }
        it = cache.insert(locale.name(), p.isEmpty() ? QStringLiteral("HH:mm") : p);
    }
    return *it;
}

QString LocalePatterns::dateRange(const QLocale &locale, const QDate &first, const QDate &last, bool brief)
{
    const QString fallback = first == last
        ? locale.toString(first, QLocale::LongFormat)
        : locale.toString(first, QLocale::LongFormat) + QStringLiteral(" – ") + locale.toString(last, QLocale::LongFormat);
    if (!first.isValid() || !last.isValid())
        return {};
    UErrorCode err = U_ZERO_ERROR;
    static const UChar utc[] = {u'U', u'T', u'C', 0};
    std::unique_ptr<UDateIntervalFormat, decltype(&udtitvfmt_close)> fmt(
        udtitvfmt_open(locale.name().toLatin1().constData(),
                       reinterpret_cast<const UChar *>(!brief ? u"yMMMMd" : first == last ? u"yMMMEd" : u"yMMMd"), -1, utc, -1,
                       &err),
        udtitvfmt_close);
    if (U_FAILURE(err))
        return fallback;
    // Noon, so that no zone rule can move either date.
    const auto noon = [](const QDate &d) { return UDate(QDateTime(d, QTime(12, 0), QTimeZone::UTC).toMSecsSinceEpoch()); };
    UChar buf[128];
    const int32_t n = udtitvfmt_format(fmt.get(), noon(first), noon(last), buf, 128, nullptr, &err);
    if (U_FAILURE(err) || n <= 0 || n > 128)
        return fallback;
    return QString(reinterpret_cast<const QChar *>(buf), n);
}

} // namespace e3
