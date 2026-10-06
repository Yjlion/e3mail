// SPDX-License-Identifier: MPL-2.0
#include "LocalePatterns.h"

#include <QHash>
#include <QMutex>

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

} // namespace e3
