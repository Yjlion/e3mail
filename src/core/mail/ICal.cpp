// SPDX-License-Identifier: MPL-2.0
#include "ICal.h"

#include "mime/Headers.h"

#include <QHash>
#include <QRegularExpression>

namespace e3::mail {

namespace {

struct Line
{
    QByteArray name;                      // upper case
    QList<QPair<QByteArray, QByteArray>> params; // upper-case names, values unquoted
    QByteArray value;                     // still escaped

    QByteArray param(const char *key) const
    {
        for (const auto &[k, v] : params) {
            if (k == key)
                return v;
        }
        return {};
    }
};

// Physical lines into logical ones (RFC 5545, 3.1): a line starting with a
// space or tab continues the one before.
QList<QByteArray> unfold(const QByteArray &data)
{
    QList<QByteArray> out;
    for (QByteArray line : data.split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        if (!out.isEmpty() && (line.startsWith(' ') || line.startsWith('\t'))) {
            out.last() += line.mid(1);
            continue;
        }
        out.append(line);
    }
    return out;
}

std::optional<Line> parseLine(const QByteArray &raw)
{
    qsizetype colon = -1;
    bool quoted = false;
    for (qsizetype i = 0; i < raw.size(); ++i) {
        if (raw[i] == '"')
            quoted = !quoted;
        else if (raw[i] == ':' && !quoted) {
            colon = i;
            break;
        }
    }
    if (colon <= 0)
        return std::nullopt;
    Line l;
    l.value = raw.mid(colon + 1);
    // Parameters: ';' outside quotes separates them.
    const QByteArray head = raw.left(colon);
    QList<QByteArray> parts;
    QByteArray cur;
    quoted = false;
    for (const char c : head) {
        if (c == '"')
            quoted = !quoted;
        if (c == ';' && !quoted) {
            parts.append(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.append(cur);
    l.name = parts.first().trimmed().toUpper();
    for (qsizetype i = 1; i < parts.size(); ++i) {
        const qsizetype eq = parts[i].indexOf('=');
        if (eq <= 0)
            continue;
        QByteArray val = parts[i].mid(eq + 1).trimmed();
        if (val.startsWith('"') && val.endsWith('"') && val.size() >= 2)
            val = val.mid(1, val.size() - 2);
        l.params.append({parts[i].left(eq).trimmed().toUpper(), val});
    }
    return l;
}

QString unescape(const QByteArray &value)
{
    const QString s = QString::fromUtf8(value);
    QString out;
    out.reserve(s.size());
    for (qsizetype i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (c == u'\\' && i + 1 < s.size()) {
            const QChar n = s[++i];
            out += (n == u'n' || n == u'N') ? QChar(u'\n') : n;
        } else {
            out += c;
        }
    }
    return out;
}

QString addrOf(const QByteArray &value)
{
    QString v = QString::fromUtf8(value).trimmed();
    if (v.startsWith(QLatin1String("mailto:"), Qt::CaseInsensitive))
        v = v.mid(7);
    v = mime::normalizeAddr(v);
    return v.contains(u'@') ? v : QString();
}

// "20261103T084000Z", "20261103T084000" (floating or with TZID), "20261103".
QDateTime parseTime(const QByteArray &value, const QByteArray &tzidParam, bool valueIsDate,
                    const QHash<QString, int> &fallbackOffsets, bool *allDay, QString *tzidOut)
{
    const QByteArray v = value.trimmed();
    if (allDay)
        *allDay = false;
    if (valueIsDate || v.size() == 8) {
        const QDate d = QDate::fromString(QString::fromLatin1(v.left(8)), QStringLiteral("yyyyMMdd"));
        if (!d.isValid())
            return {};
        if (allDay)
            *allDay = true;
        return QDateTime(d, QTime(0, 0), QTimeZone::UTC);
    }
    if (v.size() < 15 || v[8] != 'T')
        return {};
    const QDate d = QDate::fromString(QString::fromLatin1(v.left(8)), QStringLiteral("yyyyMMdd"));
    const QTime t = QTime::fromString(QString::fromLatin1(v.mid(9, 6)), QStringLiteral("HHmmss"));
    if (!d.isValid() || !t.isValid())
        return {};
    if (v.endsWith('Z'))
        return QDateTime(d, t, QTimeZone::UTC);
    if (!tzidParam.isEmpty()) {
        QByteArray id = tzidParam;
        if (id.startsWith('/')) // "/mozilla.org/20050126_1/Europe/Berlin"
            id = id.mid(id.lastIndexOf('/', id.lastIndexOf('/') - 1) + 1);
        if (!QTimeZone::isTimeZoneIdAvailable(id)) {
            const QByteArray iana = QTimeZone::windowsIdToDefaultIanaId(id);
            if (!iana.isEmpty())
                id = iana;
        }
        if (QTimeZone::isTimeZoneIdAvailable(id)) {
            if (tzidOut)
                *tzidOut = QString::fromLatin1(id);
            return QDateTime(d, t, QTimeZone(id)).toUTC();
        }
        // An unknown zone: the offset its VTIMEZONE declared, if any.
        const auto it = fallbackOffsets.constFind(QString::fromUtf8(tzidParam));
        if (it != fallbackOffsets.constEnd())
            return QDateTime(d, t, QTimeZone::fromSecondsAheadOfUtc(*it)).toUTC();
    }
    // Floating: the reader's own time.
    return QDateTime(d, t).toUTC();
}

std::optional<int> parseOffset(const QByteArray &v)
{
    // "+0100", "-0530", "+013000"
    if (v.size() < 5 || (v[0] != '+' && v[0] != '-'))
        return std::nullopt;
    bool ok1 = false, ok2 = false, ok3 = true;
    const int h = v.mid(1, 2).toInt(&ok1);
    const int m = v.mid(3, 2).toInt(&ok2);
    const int s = v.size() >= 7 ? v.mid(5, 2).toInt(&ok3) : 0;
    if (!ok1 || !ok2 || !ok3)
        return std::nullopt;
    const int total = h * 3600 + m * 60 + s;
    return v[0] == '-' ? -total : total;
}

// "-PT15M", "-P1D", "-PT1H30M", "PT0S": minutes before the start, or nullopt
// when it is after the start or not a duration.
std::optional<int> parseTriggerMinutes(const QByteArray &v)
{
    static const QRegularExpression re(QStringLiteral("^(-)?P(?:(\\d+)W)?(?:(\\d+)D)?(?:T(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+)S)?)?$"));
    const auto m = re.match(QString::fromLatin1(v.trimmed()));
    if (!m.hasMatch())
        return std::nullopt;
    const qint64 minutes = m.captured(2).toLongLong() * 7 * 24 * 60 + m.captured(3).toLongLong() * 24 * 60
        + m.captured(4).toLongLong() * 60 + m.captured(5).toLongLong() + (m.captured(6).toLongLong() + 59) / 60;
    if (minutes > 0 && m.captured(1).isEmpty())
        return std::nullopt; // after the start
    return int(qMin<qint64>(minutes, 60 * 24 * 366));
}

qint64 parseDurationMs(const QByteArray &v)
{
    static const QRegularExpression re(QStringLiteral("^(-)?P(?:(\\d+)W)?(?:(\\d+)D)?(?:T(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+)S)?)?$"));
    const auto m = re.match(QString::fromLatin1(v.trimmed()));
    if (!m.hasMatch() || !m.captured(1).isEmpty())
        return 0;
    const qint64 s = m.captured(2).toLongLong() * 7 * 86400 + m.captured(3).toLongLong() * 86400
        + m.captured(4).toLongLong() * 3600 + m.captured(5).toLongLong() * 60 + m.captured(6).toLongLong();
    return qMin<qint64>(s, 366LL * 86400) * 1000;
}

const char *dayName(Qt::DayOfWeek d)
{
    switch (d) {
    case Qt::Monday: return "MO";
    case Qt::Tuesday: return "TU";
    case Qt::Wednesday: return "WE";
    case Qt::Thursday: return "TH";
    case Qt::Friday: return "FR";
    case Qt::Saturday: return "SA";
    case Qt::Sunday: return "SU";
    }
    return "MO";
}

std::optional<Qt::DayOfWeek> dayFrom(const QString &s)
{
    static const QHash<QString, Qt::DayOfWeek> days = {
        {QStringLiteral("MO"), Qt::Monday},   {QStringLiteral("TU"), Qt::Tuesday}, {QStringLiteral("WE"), Qt::Wednesday},
        {QStringLiteral("TH"), Qt::Thursday}, {QStringLiteral("FR"), Qt::Friday},  {QStringLiteral("SA"), Qt::Saturday},
        {QStringLiteral("SU"), Qt::Sunday}};
    const auto it = days.constFind(s);
    if (it == days.constEnd())
        return std::nullopt;
    return *it;
}

QByteArray formatOffset(int seconds)
{
    const int a = qAbs(seconds);
    QByteArray out = seconds < 0 ? "-" : "+";
    out += QByteArray::number(a / 3600).rightJustified(2, '0') + QByteArray::number((a % 3600) / 60).rightJustified(2, '0');
    if (a % 60)
        out += QByteArray::number(a % 60).rightJustified(2, '0');
    return out;
}

QByteArray localStamp(const QDateTime &t, const QTimeZone &zone)
{
    return t.toTimeZone(zone).toString(QStringLiteral("yyyyMMdd'T'HHmmss")).toLatin1();
}

// A VTIMEZONE other clients can apply: one observance per transition in the
// span, or a single fixed one.
QByteArray vtimezone(const QTimeZone &zone, const QDateTime &from, const QDateTime &to)
{
    QByteArray out = "BEGIN:VTIMEZONE\r\n";
    out += ICal::fold("TZID:" + zone.id());
    const auto transitions = zone.transitions(from, to);
    if (transitions.isEmpty()) {
        const int off = zone.offsetFromUtc(from);
        out += "BEGIN:STANDARD\r\nDTSTART:19700101T000000\r\n";
        out += "TZOFFSETFROM:" + formatOffset(off) + "\r\nTZOFFSETTO:" + formatOffset(off) + "\r\n";
        out += ICal::fold("TZNAME:" + zone.abbreviation(from).toUtf8());
        out += "END:STANDARD\r\n";
    } else {
        for (const QTimeZone::OffsetData &t : transitions) {
            const int before = zone.offsetFromUtc(t.atUtc.addSecs(-1));
            const bool daylight = t.daylightTimeOffset != 0;
            out += daylight ? "BEGIN:DAYLIGHT\r\n" : "BEGIN:STANDARD\r\n";
            out += "DTSTART:" + t.atUtc.addSecs(before).toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss")).toLatin1() + "\r\n";
            out += "TZOFFSETFROM:" + formatOffset(before) + "\r\nTZOFFSETTO:" + formatOffset(t.offsetFromUtc) + "\r\n";
            if (!t.abbreviation.isEmpty())
                out += ICal::fold("TZNAME:" + t.abbreviation.toUtf8());
            out += daylight ? "END:DAYLIGHT\r\n" : "END:STANDARD\r\n";
        }
    }
    return out + "END:VTIMEZONE\r\n";
}

QByteArray attendeeLine(const char *name, const ICal::Attendee &a, bool withStatus)
{
    QByteArray out = name;
    if (!a.name.isEmpty())
        out += ";CN=\"" + ICal::clean(a.name, ICal::kMaxShort, false).remove(u'"').toUtf8() + '"';
    if (withStatus) {
        out += ";ROLE=REQ-PARTICIPANT;PARTSTAT=" + ICal::partStatName(a.partstat);
        if (a.rsvp)
            out += ";RSVP=TRUE";
    }
    return ICal::fold(out + ":mailto:" + a.addr.toUtf8());
}

} // namespace

QTimeZone ICal::Event::zone() const
{
    if (!tzid.isEmpty() && QTimeZone::isTimeZoneIdAvailable(tzid.toLatin1()))
        return QTimeZone(tzid.toLatin1());
    return allDay ? QTimeZone(QTimeZone::UTC) : QTimeZone::systemTimeZone();
}

QString ICal::clean(const QString &s, int limit, bool keepNewlines)
{
    QString out;
    out.reserve(qMin<qsizetype>(s.size(), limit));
    for (const QChar c : s) {
        if (out.size() >= limit)
            break;
        if (c == u'\n' && keepNewlines)
            out += c;
        else if (c == u'\r')
            continue;
        else if (c.isSpace())
            out += u' ';
        else if (c.category() != QChar::Other_Control && c.category() != QChar::Other_Format)
            out += c; // bidi overrides and other invisible marks are dropped
    }
    if (!keepNewlines)
        return out.simplified();
    // Collapse runs of blank lines and trailing spaces, keep paragraphs.
    QStringList lines = out.split(u'\n');
    for (QString &l : lines)
        l = l.trimmed();
    QString joined = lines.join(u'\n');
    static const QRegularExpression blank(QStringLiteral("\n{3,}"));
    joined.replace(blank, QStringLiteral("\n\n"));
    return joined.trimmed();
}

QByteArray ICal::methodName(Method m)
{
    switch (m) {
    case Method::Publish: return "PUBLISH";
    case Method::Request: return "REQUEST";
    case Method::Reply: return "REPLY";
    case Method::Cancel: return "CANCEL";
    case Method::None: break;
    }
    return {};
}

ICal::Method ICal::methodFrom(const QByteArray &name)
{
    const QByteArray n = name.trimmed().toUpper();
    if (n == "PUBLISH")
        return Method::Publish;
    if (n == "REQUEST")
        return Method::Request;
    if (n == "REPLY")
        return Method::Reply;
    if (n == "CANCEL")
        return Method::Cancel;
    return Method::None;
}

QByteArray ICal::partStatName(PartStat p)
{
    switch (p) {
    case PartStat::Accepted: return "ACCEPTED";
    case PartStat::Tentative: return "TENTATIVE";
    case PartStat::Declined: return "DECLINED";
    case PartStat::NeedsAction: break;
    }
    return "NEEDS-ACTION";
}

ICal::PartStat ICal::partStatFrom(const QByteArray &name)
{
    const QByteArray n = name.trimmed().toUpper();
    if (n == "ACCEPTED")
        return PartStat::Accepted;
    if (n == "TENTATIVE")
        return PartStat::Tentative;
    if (n == "DECLINED")
        return PartStat::Declined;
    return PartStat::NeedsAction;
}

std::optional<ICal::Rule> ICal::parseRule(const QString &rrule)
{
    if (rrule.trimmed().isEmpty())
        return std::nullopt;
    Rule r;
    bool freqSeen = false;
    for (const QString &part : rrule.toUpper().split(u';', Qt::SkipEmptyParts)) {
        const QString key = part.section(u'=', 0, 0).trimmed();
        const QString val = part.section(u'=', 1).trimmed();
        if (key == QLatin1String("FREQ")) {
            freqSeen = true;
            if (val == QLatin1String("DAILY"))
                r.freq = Rule::Freq::Daily;
            else if (val == QLatin1String("WEEKLY"))
                r.freq = Rule::Freq::Weekly;
            else if (val == QLatin1String("MONTHLY"))
                r.freq = Rule::Freq::Monthly;
            else if (val == QLatin1String("YEARLY"))
                r.freq = Rule::Freq::Yearly;
            else
                return std::nullopt;
        } else if (key == QLatin1String("INTERVAL")) {
            r.interval = val.toInt();
            if (r.interval < 1 || r.interval > 1000)
                return std::nullopt;
        } else if (key == QLatin1String("COUNT")) {
            r.count = val.toInt();
            if (r.count < 1)
                return std::nullopt;
        } else if (key == QLatin1String("UNTIL")) {
            bool allDay = false;
            r.until = parseTime(val.toLatin1(), {}, false, {}, &allDay, nullptr);
            if (!r.until.isValid())
                return std::nullopt;
            if (allDay)
                r.until = r.until.addDays(1).addSecs(-1); // the whole day
        } else if (key == QLatin1String("BYDAY")) {
            for (const QString &d : val.split(u',', Qt::SkipEmptyParts)) {
                const auto day = dayFrom(d.trimmed());
                if (!day)
                    return std::nullopt; // "2MO" and the like are not understood
                if (!r.byDay.contains(*day))
                    r.byDay.append(*day);
            }
        } else if (key == QLatin1String("WKST")) {
            // The week start matters only for rules with INTERVAL > 1 and
            // BYDAY spanning it; e3mail counts weeks from the start's.
        } else {
            return std::nullopt; // BYMONTHDAY, BYSETPOS, ...: beyond this version
        }
    }
    if (!freqSeen)
        return std::nullopt;
    if (!r.byDay.isEmpty() && r.freq != Rule::Freq::Weekly)
        return std::nullopt;
    return r;
}

QString ICal::writeRule(const Rule &r)
{
    QString out = QStringLiteral("FREQ=");
    switch (r.freq) {
    case Rule::Freq::Daily: out += QLatin1String("DAILY"); break;
    case Rule::Freq::Weekly: out += QLatin1String("WEEKLY"); break;
    case Rule::Freq::Monthly: out += QLatin1String("MONTHLY"); break;
    case Rule::Freq::Yearly: out += QLatin1String("YEARLY"); break;
    }
    if (r.interval > 1)
        out += QStringLiteral(";INTERVAL=%1").arg(r.interval);
    if (r.count > 0)
        out += QStringLiteral(";COUNT=%1").arg(r.count);
    else if (r.until.isValid())
        out += QStringLiteral(";UNTIL=") + r.until.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
    if (r.freq == Rule::Freq::Weekly && !r.byDay.isEmpty()) {
        QStringList days;
        for (const Qt::DayOfWeek d : r.byDay)
            days.append(QString::fromLatin1(dayName(d)));
        out += QStringLiteral(";BYDAY=") + days.join(u',');
    }
    return out;
}

std::optional<ICal::Document> ICal::parse(const QByteArray &ics, const QByteArray &methodHint)
{
    if (ics.size() > kMaxBytes)
        return std::nullopt;
    const QList<QByteArray> lines = unfold(ics);
    // VTIMEZONE first: an unknown TZID falls back to the offset it declares.
    QHash<QString, int> fallbackOffsets;
    {
        QString tzid;
        bool inStandard = false;
        for (const QByteArray &raw : lines) {
            const auto l = parseLine(raw);
            if (!l)
                continue;
            if (l->name == "BEGIN" && l->value.trimmed().toUpper() == "VTIMEZONE")
                tzid.clear();
            else if (l->name == "TZID" && tzid.isEmpty())
                tzid = unescape(l->value).trimmed();
            else if (l->name == "BEGIN" && l->value.trimmed().toUpper() == "STANDARD")
                inStandard = true;
            else if (l->name == "END" && l->value.trimmed().toUpper() == "STANDARD")
                inStandard = false;
            else if (l->name == "TZOFFSETTO" && inStandard && !tzid.isEmpty() && !fallbackOffsets.contains(tzid)) {
                if (const auto off = parseOffset(l->value.trimmed()))
                    fallbackOffsets.insert(tzid, *off);
            }
        }
    }

    Document doc;
    bool inCalendar = false;
    int depth = 0;       // inside components we do not read
    bool inEvent = false;
    bool inAlarm = false;
    Event ev;
    QByteArray alarmTrigger;
    QByteArray alarmAction;
    qint64 durationMs = 0;
    bool anyCalendar = false;
    for (const QByteArray &raw : lines) {
        const auto l = parseLine(raw);
        if (!l)
            continue;
        const QByteArray v = l->value.trimmed();
        if (l->name == "BEGIN") {
            const QByteArray comp = v.toUpper();
            if (comp == "VCALENDAR" && !inCalendar) {
                inCalendar = true;
                anyCalendar = true;
            } else if (comp == "VEVENT" && inCalendar && !inEvent && depth == 0) {
                inEvent = true;
                ev = Event();
                durationMs = 0;
            } else if (comp == "VALARM" && inEvent && !inAlarm) {
                inAlarm = true;
                alarmTrigger.clear();
                alarmAction.clear();
            } else {
                ++depth;
            }
            continue;
        }
        if (l->name == "END") {
            const QByteArray comp = v.toUpper();
            if (comp == "VALARM" && inAlarm) {
                inAlarm = false;
                if (!ev.reminderMinutes && alarmAction != "EMAIL" && alarmAction != "AUDIO") {
                    if (const auto m = parseTriggerMinutes(alarmTrigger))
                        ev.reminderMinutes = *m;
                }
            } else if (comp == "VEVENT" && inEvent && depth == 0) {
                inEvent = false;
                if (!ev.start.isValid() || ev.uid.isEmpty())
                    continue; // not an event anyone can place
                if (!ev.end.isValid()) {
                    if (durationMs > 0)
                        ev.end = ev.start.addMSecs(durationMs);
                    else
                        ev.end = ev.allDay ? ev.start.addDays(1) : ev.start;
                }
                if (ev.end < ev.start)
                    ev.end = ev.start;
                if (doc.events.size() < kMaxEvents)
                    doc.events.append(ev);
            } else if (comp == "VCALENDAR" && inCalendar && depth == 0) {
                inCalendar = false;
            } else if (depth > 0) {
                --depth;
            }
            continue;
        }
        if (!inCalendar || depth > 0)
            continue;
        if (inAlarm) {
            if (l->name == "TRIGGER" && l->param("VALUE") != "DATE-TIME")
                alarmTrigger = v;
            else if (l->name == "ACTION")
                alarmAction = v.toUpper();
            continue;
        }
        if (!inEvent) {
            if (l->name == "METHOD")
                doc.method = methodFrom(v);
            continue;
        }
        const bool isDate = l->param("VALUE").toUpper() == "DATE";
        if (l->name == "UID") {
            ev.uid = clean(unescape(v), kMaxShort, false);
        } else if (l->name == "SUMMARY") {
            ev.summary = clean(unescape(v), kMaxShort, false);
        } else if (l->name == "DESCRIPTION") {
            ev.description = clean(unescape(v), kMaxText, true);
        } else if (l->name == "LOCATION") {
            ev.location = clean(unescape(v), kMaxShort, false);
        } else if (l->name == "URL") {
            const QString u = clean(unescape(v), kMaxShort, false);
            if (u.startsWith(QLatin1String("https://"), Qt::CaseInsensitive) || u.startsWith(QLatin1String("http://"), Qt::CaseInsensitive))
                ev.url = u;
        } else if (l->name == "DTSTART") {
            bool allDay = false;
            QString tzid;
            ev.start = parseTime(v, l->param("TZID"), isDate, fallbackOffsets, &allDay, &tzid);
            ev.allDay = allDay;
            if (!tzid.isEmpty())
                ev.tzid = tzid;
        } else if (l->name == "DTEND") {
            bool allDay = false;
            ev.end = parseTime(v, l->param("TZID"), isDate, fallbackOffsets, &allDay, nullptr);
        } else if (l->name == "DURATION") {
            durationMs = parseDurationMs(v);
        } else if (l->name == "RRULE") {
            ev.rrule = clean(QString::fromLatin1(v), kMaxShort, false);
        } else if (l->name == "EXDATE") {
            for (const QByteArray &one : v.split(',')) {
                if (ev.exdates.size() >= kMaxExdates)
                    break;
                const QDateTime t = parseTime(one, l->param("TZID"), isDate, fallbackOffsets, nullptr, nullptr);
                if (t.isValid())
                    ev.exdates.append(t);
            }
        } else if (l->name == "RECURRENCE-ID") {
            ev.recurrenceId = parseTime(v, l->param("TZID"), isDate, fallbackOffsets, nullptr, nullptr);
        } else if (l->name == "ORGANIZER") {
            ev.organizer.addr = addrOf(v);
            ev.organizer.name = clean(unescape(l->param("CN")), kMaxShort, false);
        } else if (l->name == "ATTENDEE") {
            if (ev.attendees.size() >= kMaxAttendees)
                continue;
            Attendee a;
            a.addr = addrOf(v);
            if (a.addr.isEmpty())
                continue;
            a.name = clean(unescape(l->param("CN")), kMaxShort, false);
            a.partstat = partStatFrom(l->param("PARTSTAT"));
            a.rsvp = l->param("RSVP").toUpper() == "TRUE";
            bool dup = false;
            for (const Attendee &x : ev.attendees)
                dup = dup || x.addr == a.addr;
            if (!dup)
                ev.attendees.append(a);
        } else if (l->name == "SEQUENCE") {
            ev.sequence = qBound(0, v.toInt(), 1000000);
        } else if (l->name == "STATUS") {
            const QByteArray s = v.toUpper();
            ev.status = s == "CANCELLED" ? Status::Cancelled : s == "TENTATIVE" ? Status::Tentative : Status::Confirmed;
        } else if (l->name == "DTSTAMP") {
            ev.stamp = parseTime(v, {}, false, {}, nullptr, nullptr);
        }
    }
    if (!anyCalendar)
        return std::nullopt;
    if (doc.method == Method::None)
        doc.method = methodFrom(methodHint);
    // All-day events keep whole days; a timed end before its start is nonsense.
    for (Event &e : doc.events) {
        if (e.allDay) {
            e.start = QDateTime(e.start.toUTC().date(), QTime(0, 0), QTimeZone::UTC);
            e.end = QDateTime(e.end.toUTC().date(), QTime(0, 0), QTimeZone::UTC);
            if (e.end <= e.start)
                e.end = e.start.addDays(1);
        }
    }
    return doc;
}

QString ICal::accept(const QByteArray &ics, const QByteArray &methodHint)
{
    const QByteArray trimmed = ics.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > kMaxBytes)
        return {};
    const auto doc = parse(trimmed, methodHint);
    if (!doc || doc->events.isEmpty())
        return {};
    // Kept as text: UTF-8 that is not valid is replaced, never stored as is.
    return QString::fromUtf8(trimmed);
}

QByteArray ICal::escapeText(const QString &s)
{
    QString out = s;
    out.replace(u'\\', QStringLiteral("\\\\"));
    out.replace(u';', QStringLiteral("\\;"));
    out.replace(u',', QStringLiteral("\\,"));
    out.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    out.replace(u'\n', QStringLiteral("\\n"));
    return out.toUtf8();
}

// Folded at 75 octets (RFC 5545, 3.1), never inside a UTF-8 sequence.
QByteArray ICal::fold(const QByteArray &line)
{
    QByteArray out;
    qsizetype start = 0;
    int limit = 75;
    while (line.size() - start > limit) {
        qsizetype cut = start + limit;
        while (cut > start && (quint8(line[cut]) & 0xC0) == 0x80)
            --cut;
        out += line.mid(start, cut - start) + "\r\n ";
        start = cut;
        limit = 74;
    }
    return out + line.mid(start) + "\r\n";
}

QByteArray ICal::timeLine(const QByteArray &name, const QDateTime &t, bool allDay, const QString &tzid)
{
    if (allDay)
        return fold(name + ";VALUE=DATE:" + t.toUTC().date().toString(QStringLiteral("yyyyMMdd")).toLatin1());
    if (!tzid.isEmpty() && QTimeZone::isTimeZoneIdAvailable(tzid.toLatin1()))
        return fold(name + ";TZID=" + tzid.toLatin1() + ":" + localStamp(t, QTimeZone(tzid.toLatin1())));
    return fold(name + ":" + t.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")).toLatin1());
}

QByteArray ICal::write(const Document &doc)
{
    QByteArray out = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//e3mail//calendar//EN\r\n";
    if (doc.method != Method::None)
        out += "METHOD:" + methodName(doc.method) + "\r\n";
    // A zone travels only where a recurrence needs it; single events are UTC.
    QStringList zones;
    for (const Event &e : doc.events) {
        if (!e.allDay && !e.rrule.isEmpty() && !e.tzid.isEmpty() && QTimeZone::isTimeZoneIdAvailable(e.tzid.toLatin1())
            && !zones.contains(e.tzid))
            zones.append(e.tzid);
    }
    for (const QString &z : zones) {
        QDateTime from = QDateTime::currentDateTimeUtc().addYears(-1);
        QDateTime to = from.addYears(4);
        for (const Event &e : doc.events) {
            if (e.tzid == z && e.start.isValid()) {
                from = qMin(from, e.start.addYears(-1));
                to = qMax(to, e.start.addYears(3));
            }
        }
        out += vtimezone(QTimeZone(z.toLatin1()), from, to);
    }
    for (const Event &e : doc.events) {
        const QString zone = e.allDay || e.rrule.isEmpty() ? QString() : e.tzid;
        out += "BEGIN:VEVENT\r\n";
        out += fold("UID:" + escapeText(e.uid));
        const QDateTime stamp = e.stamp.isValid() ? e.stamp : QDateTime::currentDateTimeUtc();
        out += "DTSTAMP:" + stamp.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")).toLatin1() + "\r\n";
        if (e.sequence > 0)
            out += "SEQUENCE:" + QByteArray::number(e.sequence) + "\r\n";
        out += timeLine("DTSTART", e.start, e.allDay, zone);
        if (e.end.isValid())
            out += timeLine("DTEND", e.end, e.allDay, zone);
        out += fold("SUMMARY:" + escapeText(e.summary));
        if (!e.description.isEmpty())
            out += fold("DESCRIPTION:" + escapeText(e.description));
        if (!e.location.isEmpty())
            out += fold("LOCATION:" + escapeText(e.location));
        if (!e.url.isEmpty())
            out += fold("URL:" + e.url.toUtf8());
        if (e.status == Status::Cancelled)
            out += "STATUS:CANCELLED\r\n";
        else if (e.status == Status::Tentative)
            out += "STATUS:TENTATIVE\r\n";
        if (!e.rrule.isEmpty())
            out += fold("RRULE:" + e.rrule.toLatin1());
        for (const QDateTime &x : e.exdates)
            out += timeLine("EXDATE", x, e.allDay, zone);
        if (!e.organizer.addr.isEmpty())
            out += attendeeLine("ORGANIZER", e.organizer, false);
        for (const Attendee &a : e.attendees)
            out += attendeeLine("ATTENDEE", a, true);
        if (e.reminderMinutes && *e.reminderMinutes >= 0) {
            out += "BEGIN:VALARM\r\nACTION:DISPLAY\r\nDESCRIPTION:Reminder\r\n";
            out += "TRIGGER:-PT" + QByteArray::number(*e.reminderMinutes) + "M\r\nEND:VALARM\r\n";
        }
        out += "END:VEVENT\r\n";
    }
    return out + "END:VCALENDAR\r\n";
}

QList<QDateTime> ICal::occurrences(const Event &e, const QDateTime &from, const QDateTime &to, int limit)
{
    QList<QDateTime> out;
    if (!e.start.isValid() || !from.isValid() || !to.isValid() || to <= from || limit <= 0)
        return out;
    const qint64 dur = qMax<qint64>(0, e.durationMs());
    auto overlaps = [&](const QDateTime &s) { return s < to && s.addMSecs(dur) > from; };
    auto excluded = [&](const QDateTime &s) {
        for (const QDateTime &x : e.exdates) {
            if (e.allDay ? x.toUTC().date() == s.toUTC().date() : x == s)
                return true;
        }
        return false;
    };
    const auto rule = e.rule();
    if (!rule) {
        if (overlaps(e.start) && !excluded(e.start))
            out.append(e.start);
        return out;
    }
    const QTimeZone zone = e.zone();
    const QDateTime hardEnd = qMin(to, from.addYears(2));
    const QDateTime localStart = e.start.toTimeZone(zone);
    const QTime wall = localStart.time();
    int produced = 0; // instances the rule produced, for COUNT
    int steps = 0;
    auto consider = [&](const QDateTime &s) -> bool {
        // Returns false when the series is over.
        if (s < e.start)
            return true;
        if (rule->until.isValid() && s > rule->until)
            return false;
        ++produced;
        if (rule->count > 0 && produced > rule->count)
            return false;
        if (s >= hardEnd)
            return false;
        if (overlaps(s) && !excluded(s))
            out.append(s.toUTC());
        return out.size() < limit;
    };
    if (rule->freq == Rule::Freq::Weekly) {
        QList<Qt::DayOfWeek> days = rule->byDay;
        if (days.isEmpty())
            days.append(Qt::DayOfWeek(localStart.date().dayOfWeek()));
        std::sort(days.begin(), days.end());
        // Weeks start on Monday; the first week is the start's.
        QDate monday = localStart.date().addDays(1 - localStart.date().dayOfWeek());
        for (;;) {
            if (++steps > 100000)
                break;
            bool go = true;
            for (const Qt::DayOfWeek d : days) {
                const QDate date = monday.addDays(int(d) - 1);
                const QDateTime s(date, wall, zone);
                if (!consider(s)) {
                    go = false;
                    break;
                }
            }
            if (!go)
                break;
            monday = monday.addDays(7 * rule->interval);
            if (QDateTime(monday, wall, zone) >= hardEnd && produced > 0)
                break;
        }
        return out;
    }
    const QDate startDate = localStart.date();
    for (int i = 0;; ++i) {
        if (++steps > 100000)
            break;
        QDate date;
        switch (rule->freq) {
        case Rule::Freq::Daily: date = startDate.addDays(qint64(i) * rule->interval); break;
        case Rule::Freq::Monthly: {
            const QDate m = QDate(startDate.year(), startDate.month(), 1).addMonths(i * rule->interval);
            if (startDate.day() > m.daysInMonth())
                continue; // no 31st this month: skipped, as the RFC says
            date = QDate(m.year(), m.month(), startDate.day());
            break;
        }
        case Rule::Freq::Yearly: {
            const int y = startDate.year() + i * rule->interval;
            if (!QDate::isValid(y, startDate.month(), startDate.day()))
                continue; // 29 February
            date = QDate(y, startDate.month(), startDate.day());
            break;
        }
        case Rule::Freq::Weekly: break;
        }
        if (!date.isValid())
            break;
        const QDateTime s(date, wall, zone);
        if (!consider(s))
            break;
        if (s >= hardEnd)
            break;
    }
    return out;
}

} // namespace e3::mail
