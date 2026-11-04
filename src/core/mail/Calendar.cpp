// SPDX-License-Identifier: MPL-2.0
#include "Calendar.h"

#include "Compose.h"
#include "Contacts.h"
#include "Policy.h"
#include "Search.h"
#include "mime/Headers.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"
#include "util/Log.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QUuid>

namespace e3::mail {

namespace {

constexpr const char *kSelect = "SELECT id, uid, summary, description, location, url, start_ms, end_ms, all_day, tzid, "
                                "rrule, exdates, organizer, organizer_name, sequence, status, reminder_min, source_mid, "
                                "stamp_ms, updated_ms FROM events ";

QDateTime fromMs(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC);
}

QString selfAddr(MailContext &ctx)
{
    return mime::normalizeAddr(ctx.config.get(cfg::Addr));
}

QList<QDateTime> exdatesFrom(const QString &json)
{
    QList<QDateTime> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(json.toUtf8()).array())
        out.append(fromMs(qint64(v.toDouble())));
    return out;
}

QString exdatesJson(const QList<QDateTime> &dates)
{
    QJsonArray a;
    for (const QDateTime &d : dates)
        a.append(double(d.toMSecsSinceEpoch()));
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}

void readAttendees(MailContext &ctx, Calendar::Event &e)
{
    Statement st(ctx.db, "SELECT addr, name, partstat, replied_ms FROM event_attendees WHERE event_id=? ORDER BY rowid");
    st.bind(1, e.id);
    while (st.step()) {
        ICal::Attendee a;
        a.addr = st.text(0);
        a.name = st.text(1);
        a.partstat = ICal::PartStat(qBound(0, st.integer(2), 3));
        a.rsvp = true;
        e.attendees.append(a);
        if (!st.isNull(3))
            e.replied.insert(a.addr, fromMs(st.int64(3)));
    }
}

Calendar::Event readRow(MailContext &ctx, Statement &st)
{
    Calendar::Event e;
    e.id = st.int64(0);
    e.uid = st.text(1);
    e.summary = st.text(2);
    e.description = st.text(3);
    e.location = st.text(4);
    e.url = st.text(5);
    e.start = fromMs(st.int64(6));
    e.end = fromMs(st.int64(7));
    e.allDay = st.integer(8) != 0;
    e.tzid = st.text(9);
    e.rrule = st.text(10);
    e.exdates = exdatesFrom(st.text(11));
    e.organizer.addr = st.text(12);
    e.organizer.name = st.text(13);
    e.sequence = st.integer(14);
    e.status = ICal::Status(qBound(0, st.integer(15), 2));
    if (!st.isNull(16))
        e.reminderMinutes = st.integer(16);
    e.sourceMid = st.text(17);
    e.stamp = fromMs(st.int64(18));
    e.updatedAt = fromMs(st.int64(19));
    readAttendees(ctx, e);
    return e;
}

// Normalises what a person or a payload hands over.
Calendar::Event cleaned(Calendar::Event e)
{
    e.uid = ICal::clean(e.uid, ICal::kMaxShort, false);
    e.summary = ICal::clean(e.summary, ICal::kMaxShort, false);
    e.description = ICal::clean(e.description, ICal::kMaxText, true);
    e.location = ICal::clean(e.location, ICal::kMaxShort, false);
    e.url = ICal::clean(e.url, ICal::kMaxShort, false);
    if (!e.url.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)
        && !e.url.startsWith(QLatin1String("http://"), Qt::CaseInsensitive))
        e.url.clear();
    e.tzid = ICal::clean(e.tzid, ICal::kMaxShort, false);
    e.rrule = ICal::clean(e.rrule, ICal::kMaxShort, false);
    e.organizer.addr = mime::normalizeAddr(e.organizer.addr);
    e.organizer.name = ICal::clean(e.organizer.name, ICal::kMaxShort, false);
    if (e.allDay) {
        e.start = QDateTime(e.start.toUTC().date(), QTime(0, 0), QTimeZone::UTC);
        e.end = QDateTime(e.end.toUTC().date(), QTime(0, 0), QTimeZone::UTC);
        if (e.end <= e.start)
            e.end = e.start.addDays(1);
    } else {
        e.start = e.start.toUTC();
        e.end = e.end.isValid() ? e.end.toUTC() : e.start;
        if (e.end < e.start)
            e.end = e.start;
    }
    QList<ICal::Attendee> attendees;
    for (ICal::Attendee a : e.attendees) {
        a.addr = mime::normalizeAddr(a.addr);
        a.name = ICal::clean(a.name, ICal::kMaxShort, false);
        if (!a.addr.contains(u'@') || attendees.size() >= ICal::kMaxAttendees)
            continue;
        bool dup = false;
        for (const ICal::Attendee &x : attendees)
            dup = dup || x.addr == a.addr;
        if (!dup)
            attendees.append(a);
    }
    e.attendees = attendees;
    if (e.exdates.size() > ICal::kMaxExdates)
        e.exdates = e.exdates.mid(0, ICal::kMaxExdates);
    if (e.reminderMinutes && *e.reminderMinutes < 0)
        e.reminderMinutes.reset();
    return e;
}

// Without a transaction or an op: shared by the person's own changes, ops
// from other devices, and what arriving mail derives.
qint64 writeEvent(MailContext &ctx, const Calendar::Event &e)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    ctx.db.run("INSERT INTO events(uid, summary, description, location, url, start_ms, end_ms, all_day, tzid, rrule, "
               "exdates, organizer, organizer_name, sequence, status, reminder_min, source_mid, stamp_ms, updated_ms) "
               "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
               "ON CONFLICT(uid) DO UPDATE SET summary=excluded.summary, description=excluded.description, "
               "location=excluded.location, url=excluded.url, start_ms=excluded.start_ms, end_ms=excluded.end_ms, "
               "all_day=excluded.all_day, tzid=excluded.tzid, rrule=excluded.rrule, exdates=excluded.exdates, "
               "organizer=excluded.organizer, organizer_name=excluded.organizer_name, sequence=excluded.sequence, "
               "status=excluded.status, reminder_min=excluded.reminder_min, "
               "source_mid=coalesce(excluded.source_mid, events.source_mid), stamp_ms=excluded.stamp_ms, "
               "updated_ms=excluded.updated_ms",
               e.uid, e.summary, e.description, e.location, e.url, e.start.toMSecsSinceEpoch(),
               e.end.toMSecsSinceEpoch(), e.allDay, e.tzid, e.rrule, exdatesJson(e.exdates), e.organizer.addr,
               e.organizer.name, e.sequence, int(e.status), e.reminderMinutes, nullIfEmpty(e.sourceMid),
               e.stamp.isValid() ? e.stamp.toMSecsSinceEpoch() : now, now);
    const qint64 id = ctx.db.queryInt("SELECT id FROM events WHERE uid=?", e.uid).value_or(0);
    ctx.db.run("DELETE FROM event_attendees WHERE event_id=?", id);
    for (const ICal::Attendee &a : e.attendees) {
        const auto r = e.replied.constFind(a.addr);
        ctx.db.run("INSERT INTO event_attendees(event_id, addr, name, partstat, replied_ms) VALUES(?, ?, ?, ?, ?)", id,
                   a.addr, a.name, int(a.partstat),
                   r == e.replied.constEnd() ? std::optional<qint64>() : std::optional<qint64>(r->toMSecsSinceEpoch()));
    }
    return id;
}

QString newUid(MailContext &ctx)
{
    const QString self = selfAddr(ctx);
    return QUuid::createUuid().toString(QUuid::WithoutBraces) + u'@' + self.section(u'@', 1);
}

} // namespace

QList<Calendar::Occurrence> Calendar::list(MailContext &ctx, const QDateTime &from, const QDateTime &to)
{
    QList<Occurrence> out;
    const QString self = selfAddr(ctx);
    // Everything that starts before the window ends; recurrence decides the rest.
    Statement st(ctx.db, (QByteArray(kSelect) + "WHERE start_ms < ? AND status <> 2 ORDER BY start_ms").constData());
    st.bind(1, to.toMSecsSinceEpoch());
    while (st.step()) {
        const Event e = readRow(ctx, st);
        if (e.rrule.isEmpty() && e.end <= from)
            continue;
        if (const auto mine = ourStatus(ctx, e); mine && *mine == ICal::PartStat::Declined)
            continue;
        const qint64 dur = e.durationMs();
        for (const QDateTime &s : ICal::occurrences(e, from, to))
            out.append({e, s, s.addMSecs(dur)});
    }
    std::stable_sort(out.begin(), out.end(), [](const Occurrence &a, const Occurrence &b) { return a.start < b.start; });
    return out;
}

std::optional<Calendar::Event> Calendar::get(MailContext &ctx, const QString &uid)
{
    Statement st(ctx.db, (QByteArray(kSelect) + "WHERE uid=?").constData());
    st.bind(1, uid);
    if (!st.step())
        return std::nullopt;
    return readRow(ctx, st);
}

std::optional<Calendar::Event> Calendar::byId(MailContext &ctx, qint64 id)
{
    Statement st(ctx.db, (QByteArray(kSelect) + "WHERE id=?").constData());
    st.bind(1, id);
    if (!st.step())
        return std::nullopt;
    return readRow(ctx, st);
}

bool Calendar::organizedByUs(MailContext &ctx, const Event &e)
{
    return !e.organizer.addr.isEmpty() && e.organizer.addr == selfAddr(ctx);
}

std::optional<ICal::PartStat> Calendar::ourStatus(MailContext &ctx, const Event &e)
{
    if (organizedByUs(ctx, e))
        return std::nullopt;
    const QString self = selfAddr(ctx);
    for (const ICal::Attendee &a : e.attendees) {
        if (a.addr == self)
            return a.partstat;
    }
    return std::nullopt;
}

QByteArray Calendar::exportIcs(MailContext &ctx, const QString &uid)
{
    const auto e = get(ctx, uid);
    if (!e)
        return {};
    return ICal::write({ICal::Method::Publish, {*e}});
}

QJsonObject Calendar::payload(const Event &e)
{
    QJsonArray attendees;
    for (const ICal::Attendee &a : e.attendees) {
        QJsonObject o{{QStringLiteral("addr"), a.addr},
                      {QStringLiteral("name"), a.name},
                      {QStringLiteral("partstat"), int(a.partstat)}};
        if (const auto r = e.replied.constFind(a.addr); r != e.replied.constEnd())
            o.insert(QStringLiteral("replied"), double(r->toMSecsSinceEpoch()));
        attendees.append(o);
    }
    QJsonArray exdates;
    for (const QDateTime &d : e.exdates)
        exdates.append(double(d.toMSecsSinceEpoch()));
    QJsonObject p{{QStringLiteral("uid"), e.uid},
                  {QStringLiteral("summary"), e.summary},
                  {QStringLiteral("description"), e.description},
                  {QStringLiteral("location"), e.location},
                  {QStringLiteral("url"), e.url},
                  {QStringLiteral("start"), double(e.start.toMSecsSinceEpoch())},
                  {QStringLiteral("end"), double(e.end.toMSecsSinceEpoch())},
                  {QStringLiteral("allDay"), e.allDay},
                  {QStringLiteral("tzid"), e.tzid},
                  {QStringLiteral("rrule"), e.rrule},
                  {QStringLiteral("exdates"), exdates},
                  {QStringLiteral("organizer"), e.organizer.addr},
                  {QStringLiteral("organizerName"), e.organizer.name},
                  {QStringLiteral("sequence"), e.sequence},
                  {QStringLiteral("status"), int(e.status)},
                  {QStringLiteral("sourceMid"), e.sourceMid},
                  {QStringLiteral("stamp"), double(e.stamp.isValid() ? e.stamp.toMSecsSinceEpoch() : 0)},
                  {QStringLiteral("attendees"), attendees}};
    if (e.reminderMinutes)
        p.insert(QStringLiteral("reminder"), *e.reminderMinutes);
    return p;
}

Calendar::Event Calendar::fromPayload(const QJsonObject &p)
{
    auto s = [&p](const char *k) { return p.value(QLatin1String(k)).toString(); };
    auto ms = [&p](const char *k) { return fromMs(qint64(p.value(QLatin1String(k)).toDouble())); };
    Event e;
    e.uid = s("uid");
    e.summary = s("summary");
    e.description = s("description");
    e.location = s("location");
    e.url = s("url");
    e.start = ms("start");
    e.end = ms("end");
    e.allDay = p.value(QLatin1String("allDay")).toBool();
    e.tzid = s("tzid");
    e.rrule = s("rrule");
    for (const QJsonValue &v : p.value(QLatin1String("exdates")).toArray())
        e.exdates.append(fromMs(qint64(v.toDouble())));
    e.organizer.addr = s("organizer");
    e.organizer.name = s("organizerName");
    e.sequence = p.value(QLatin1String("sequence")).toInt();
    e.status = ICal::Status(qBound(0, p.value(QLatin1String("status")).toInt(), 2));
    if (p.contains(QLatin1String("reminder")))
        e.reminderMinutes = p.value(QLatin1String("reminder")).toInt();
    e.sourceMid = s("sourceMid");
    if (p.value(QLatin1String("stamp")).toDouble() > 0)
        e.stamp = ms("stamp");
    for (const QJsonValue &v : p.value(QLatin1String("attendees")).toArray()) {
        const QJsonObject o = v.toObject();
        ICal::Attendee a;
        a.addr = o.value(QLatin1String("addr")).toString();
        a.name = o.value(QLatin1String("name")).toString();
        a.partstat = ICal::PartStat(qBound(0, o.value(QLatin1String("partstat")).toInt(), 3));
        a.rsvp = true;
        e.attendees.append(a);
        if (o.contains(QLatin1String("replied")))
            e.replied.insert(a.addr, fromMs(qint64(o.value(QLatin1String("replied")).toDouble())));
    }
    return cleaned(e);
}

QString Calendar::saveLocal(MailContext &ctx, Event e)
{
    if (e.uid.isEmpty())
        e.uid = newUid(ctx);
    e = cleaned(e);
    if (!e.stamp.isValid())
        e.stamp = QDateTime::currentDateTimeUtc();
    Transaction tx(ctx.db);
    writeEvent(ctx, e);
    ctx.ops.record(op::CalendarEvent, payload(e));
    tx.commit();
    return e.uid;
}

void Calendar::removeLocal(MailContext &ctx, const QString &uid)
{
    Transaction tx(ctx.db);
    ctx.db.run("DELETE FROM events WHERE uid=?", uid);
    ctx.ops.record(op::CalendarRemove, {{QStringLiteral("uid"), uid}});
    tx.commit();
}

QString Calendar::addEvent(MailContext &ctx, const ICal::Event &ical, const QString &sourceMid)
{
    Event e;
    static_cast<ICal::Event &>(e) = ical;
    e.sourceMid = sourceMid;
    e.attendees.clear();
    e.organizer = {};
    e.sequence = 0;
    e.status = ICal::Status::Confirmed;
    if (!e.reminderMinutes) {
        const int d = ctx.config.getInt(cfg::CalendarReminder);
        if (d >= 0)
            e.reminderMinutes = d;
    }
    if (e.uid.isEmpty())
        e.uid = newUid(ctx);
    return saveLocal(ctx, e);
}

bool Calendar::applyEvent(MailContext &ctx, const QJsonObject &p)
{
    Event incoming = fromPayload(p);
    if (incoming.uid.isEmpty() || !incoming.start.isValid())
        return false;
    if (const auto have = get(ctx, incoming.uid)) {
        // A newer version from the organizer got here first (by mail): keep it.
        if (incoming.sequence < have->sequence)
            return false;
        // Each attendee's answer is whichever side heard the newer one.
        for (ICal::Attendee &a : incoming.attendees) {
            const auto theirs = incoming.replied.constFind(a.addr);
            const auto ours = have->replied.constFind(a.addr);
            if (ours == have->replied.constEnd())
                continue;
            if (theirs == incoming.replied.constEnd() || *theirs < *ours) {
                for (const ICal::Attendee &h : have->attendees) {
                    if (h.addr == a.addr) {
                        a.partstat = h.partstat;
                        incoming.replied.insert(a.addr, *ours);
                    }
                }
            }
        }
    }
    writeEvent(ctx, incoming);
    rederive(ctx, incoming.uid);
    return true;
}

void Calendar::applyRemove(MailContext &ctx, const QString &uid)
{
    ctx.db.run("DELETE FROM events WHERE uid=?", uid);
}

void Calendar::rederive(MailContext &ctx, const QString &uid)
{
    auto e = get(ctx, uid);
    if (!e || !organizedByUs(ctx, *e))
        return;
    Statement st(ctx.db, "SELECT calendar, from_addr FROM messages WHERE cal_uid=? AND cal_method='REPLY' "
                         "AND direction=0 AND hidden=0 ORDER BY date");
    st.bind(1, uid);
    bool changed = false;
    while (st.step()) {
        const auto doc = ICal::parse(st.text(0).toUtf8(), "REPLY");
        if (!doc)
            continue;
        const QString from = mime::normalizeAddr(st.text(1));
        for (const ICal::Event &r : doc->events) {
            if (r.uid != uid)
                continue;
            for (const ICal::Attendee &a : r.attendees) {
                if (a.addr != from)
                    continue; // only the sender's own answer counts
                for (ICal::Attendee &mine : e->attendees) {
                    if (mine.addr != a.addr)
                        continue;
                    const QDateTime at = r.stamp.isValid() ? r.stamp : QDateTime::currentDateTimeUtc();
                    // DTSTAMP has whole seconds: of two answers stamped alike,
                    // the message that came later (they are read in date order) wins.
                    const auto have = e->replied.constFind(a.addr);
                    if (have != e->replied.constEnd() && *have > at)
                        continue;
                    mine.partstat = a.partstat;
                    e->replied.insert(a.addr, at);
                    changed = true;
                }
            }
        }
    }
    if (changed)
        writeEvent(ctx, *e);
}

QList<Calendar::Occurrence> Calendar::dueReminders(MailContext &ctx, qint64 nowMs)
{
    QList<Occurrence> out;
    const QDateTime now = fromMs(nowMs);
    // Anything whose reminder fell in the last day and whose start is still
    // ahead (or just passed): a device that was off misses older ones.
    for (const Occurrence &o : list(ctx, now.addDays(-1), now.addDays(2))) {
        if (!o.event.reminderMinutes)
            continue;
        const QDateTime due = o.start.addSecs(-60LL * *o.event.reminderMinutes);
        if (due > now || o.start < now.addSecs(-3600))
            continue;
        if (ctx.db.queryInt("SELECT 1 FROM reminders_shown WHERE uid=? AND occurrence_ms=?", o.event.uid,
                            o.start.toMSecsSinceEpoch()))
            continue;
        ctx.db.run("INSERT OR IGNORE INTO reminders_shown(uid, occurrence_ms) VALUES(?, ?)", o.event.uid,
                   o.start.toMSecsSinceEpoch());
        out.append(o);
    }
    return out;
}

std::optional<qint64> Calendar::nextReminderMs(MailContext &ctx, qint64 nowMs)
{
    const QDateTime now = fromMs(nowMs);
    std::optional<qint64> next;
    for (const Occurrence &o : list(ctx, now, now.addDays(32))) {
        if (!o.event.reminderMinutes)
            continue;
        const qint64 due = o.start.addSecs(-60LL * *o.event.reminderMinutes).toMSecsSinceEpoch();
        if (due < nowMs)
            continue;
        if (ctx.db.queryInt("SELECT 1 FROM reminders_shown WHERE uid=? AND occurrence_ms=?", o.event.uid,
                            o.start.toMSecsSinceEpoch()))
            continue;
        if (!next || due < *next)
            next = due;
    }
    return next;
}

QString Calendar::describeRule(const QString &rrule)
{
    const auto r = ICal::parseRule(rrule);
    if (!r) {
        if (rrule.isEmpty())
            return QString();
        return QCoreApplication::translate("Calendar", "Repeats (rule not understood: %1)").arg(rrule);
    }
    QString out;
    switch (r->freq) {
    case ICal::Rule::Freq::Daily:
        out = r->interval == 1 ? QCoreApplication::translate("Calendar", "Every day")
                               : QCoreApplication::translate("Calendar", "Every %n days", "", r->interval);
        break;
    case ICal::Rule::Freq::Weekly: {
        out = r->interval == 1 ? QCoreApplication::translate("Calendar", "Every week")
                               : QCoreApplication::translate("Calendar", "Every %n weeks", "", r->interval);
        if (!r->byDay.isEmpty()) {
            QStringList days;
            for (const Qt::DayOfWeek d : r->byDay)
                days.append(QLocale().dayName(int(d), QLocale::LongFormat));
            out = QCoreApplication::translate("Calendar", "%1 on %2").arg(out, days.join(QCoreApplication::translate("Calendar", ", ")));
        }
        break;
    }
    case ICal::Rule::Freq::Monthly:
        out = r->interval == 1 ? QCoreApplication::translate("Calendar", "Every month")
                               : QCoreApplication::translate("Calendar", "Every %n months", "", r->interval);
        break;
    case ICal::Rule::Freq::Yearly:
        out = r->interval == 1 ? QCoreApplication::translate("Calendar", "Every year")
                               : QCoreApplication::translate("Calendar", "Every %n years", "", r->interval);
        break;
    }
    if (r->count > 0)
        out = QCoreApplication::translate("Calendar", "%1, %n time(s)", "", r->count).arg(out);
    else if (r->until.isValid())
        out = QCoreApplication::translate("Calendar", "%1, until %2").arg(out, QLocale().toString(r->until.toLocalTime().date(), QLocale::ShortFormat));
    return out;
}

QString Calendar::describeWhen(const ICal::Event &e, const QLocale &locale)
{
    if (e.allDay) {
        const QDate first = e.start.toUTC().date();
        const QDate last = e.end.toUTC().date().addDays(-1);
        if (last <= first)
            return locale.toString(first, QLocale::LongFormat);
        return QCoreApplication::translate("Calendar", "%1 – %2")
            .arg(locale.toString(first, QLocale::LongFormat), locale.toString(last, QLocale::LongFormat));
    }
    const QDateTime s = e.start.toLocalTime();
    const QDateTime t = e.end.toLocalTime();
    const QString day = locale.toString(s.date(), QLocale::LongFormat);
    const QString from = locale.toString(s.time(), QLocale::ShortFormat);
    if (t <= s)
        return QCoreApplication::translate("Calendar", "%1, %2").arg(day, from);
    if (t.date() == s.date())
        return QCoreApplication::translate("Calendar", "%1, %2 – %3").arg(day, from, locale.toString(t.time(), QLocale::ShortFormat));
    return QCoreApplication::translate("Calendar", "%1, %2 – %3")
        .arg(day, from, locale.toString(t, QLocale::ShortFormat));
}

// --- Invitations by mail ---

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("Calendar", text);
}

QString displayName(MailContext &ctx)
{
    const QString n = ctx.config.get(cfg::DisplayName).trimmed();
    return n.isEmpty() ? selfAddr(ctx) : n;
}

QString escapeHtml(const QString &s)
{
    return s.toHtmlEscaped();
}

QString addrDisplay(const ICal::Attendee &a)
{
    return a.name.isEmpty() ? a.addr : a.name + QStringLiteral(" <") + a.addr + u'>';
}

// Everyone but ourselves, as the mail's recipients.
QList<mime::Address> recipientsOf(const QList<ICal::Attendee> &attendees, const QString &self)
{
    QList<mime::Address> out;
    for (const ICal::Attendee &a : attendees) {
        if (a.addr != self)
            out.append({a.name, a.addr});
    }
    return out;
}

Draft draftFor(const Calendar::InviteMail &m, const QList<mime::Address> &to)
{
    Draft d;
    d.to = to;
    d.subject = m.subject;
    d.text = m.text;
    d.html = m.html;
    d.calendar = m.ics;
    d.calendarMethod = m.method;
    d.structured = m.jsonLd;
    return d;
}

QString partStatSentence(ICal::PartStat p, const QString &who, const QString &summary)
{
    switch (p) {
    case ICal::PartStat::Accepted: return tr("%1 has accepted the invitation to “%2”.").arg(who, summary);
    case ICal::PartStat::Tentative: return tr("%1 has tentatively accepted the invitation to “%2”.").arg(who, summary);
    case ICal::PartStat::Declined: return tr("%1 has declined the invitation to “%2”.").arg(who, summary);
    case ICal::PartStat::NeedsAction: break;
    }
    return tr("%1 has not answered the invitation to “%2”.").arg(who, summary);
}

} // namespace

Calendar::InviteMail Calendar::inviteMail(MailContext &ctx, const Event &e, ICal::Method method,
                                          std::optional<ICal::PartStat> answer)
{
    InviteMail m;
    m.method = ICal::methodName(method);
    const QString self = selfAddr(ctx);
    const QString when = describeWhen(e);
    const QString repeats = describeRule(e.rrule);
    QStringList lines;
    QStringList html;
    if (method == ICal::Method::Reply && answer) {
        const QString who = displayName(ctx);
        m.subject = (*answer == ICal::PartStat::Accepted ? tr("Accepted: %1")
                     : *answer == ICal::PartStat::Tentative ? tr("Tentative: %1")
                                                            : tr("Declined: %1"))
                        .arg(e.summary);
        lines.append(partStatSentence(*answer, who, e.summary));
        html.append(QStringLiteral("<p>") + escapeHtml(lines.last()) + QStringLiteral("</p>"));
        ICal::Event r;
        r.uid = e.uid;
        r.summary = e.summary;
        r.start = e.start;
        r.end = e.end;
        r.allDay = e.allDay;
        r.tzid = e.tzid;
        r.rrule = e.rrule;
        r.sequence = e.sequence;
        r.stamp = QDateTime::currentDateTimeUtc();
        r.organizer = e.organizer;
        r.attendees = {{self, displayName(ctx), *answer, false}};
        m.ics = ICal::write({ICal::Method::Reply, {r}});
    } else {
        const QString organizer = e.organizer.name.isEmpty() ? e.organizer.addr : e.organizer.name;
        if (method == ICal::Method::Cancel) {
            m.subject = tr("Cancelled: %1").arg(e.summary);
            lines.append(tr("%1 has cancelled “%2”.").arg(organizer, e.summary));
        } else if (e.sequence > 0) {
            m.subject = tr("Updated invitation: %1").arg(e.summary);
            lines.append(tr("%1 has changed “%2”. Here are the new details.").arg(organizer, e.summary));
        } else {
            m.subject = tr("Invitation: %1").arg(e.summary);
            lines.append(tr("%1 invites you to “%2”.").arg(organizer, e.summary));
        }
        html.append(QStringLiteral("<p>") + escapeHtml(lines.last()) + QStringLiteral("</p>"));
        lines.append(QString());
        QStringList facts;
        facts.append(tr("When: %1").arg(when));
        if (!repeats.isEmpty())
            facts.append(tr("Repeats: %1").arg(repeats));
        if (!e.location.isEmpty())
            facts.append(tr("Where: %1").arg(e.location));
        if (!e.url.isEmpty())
            facts.append(tr("Link: %1").arg(e.url));
        QStringList guests;
        for (const ICal::Attendee &a : e.attendees)
            guests.append(addrDisplay(a));
        if (!guests.isEmpty())
            facts.append(tr("Who: %1").arg(guests.join(QStringLiteral(", "))));
        lines += facts;
        QStringList factHtml;
        for (const QString &f : facts)
            factHtml.append(escapeHtml(f));
        html.append(QStringLiteral("<p>") + factHtml.join(QStringLiteral("<br>")) + QStringLiteral("</p>"));
        if (!e.description.isEmpty()) {
            lines.append(QString());
            lines.append(e.description);
            html.append(QStringLiteral("<p>") + escapeHtml(e.description).replace(u'\n', QStringLiteral("<br>")) + QStringLiteral("</p>"));
        }
        ICal::Event out = e;
        out.stamp = QDateTime::currentDateTimeUtc();
        if (method == ICal::Method::Cancel)
            out.status = ICal::Status::Cancelled;
        for (ICal::Attendee &a : out.attendees)
            a.rsvp = method != ICal::Method::Cancel;
        m.ics = ICal::write({method, {out}});
        if (method != ICal::Method::Cancel) {
            // The same facts as schema.org, for clients that read structured
            // mail (ADR 0024 amendment).
            QJsonObject ld{{QStringLiteral("@context"), QStringLiteral("https://schema.org")},
                           {QStringLiteral("@type"), QStringLiteral("Event")},
                           {QStringLiteral("name"), e.summary},
                           {QStringLiteral("startDate"), e.allDay ? e.start.toUTC().date().toString(Qt::ISODate) : e.start.toUTC().toString(Qt::ISODate)},
                           {QStringLiteral("endDate"), e.allDay ? e.end.toUTC().date().addDays(-1).toString(Qt::ISODate) : e.end.toUTC().toString(Qt::ISODate)}};
            if (!e.location.isEmpty())
                ld.insert(QStringLiteral("location"), QJsonObject{{QStringLiteral("@type"), QStringLiteral("Place")}, {QStringLiteral("name"), e.location}});
            if (!e.description.isEmpty())
                ld.insert(QStringLiteral("description"), e.description);
            if (!e.url.isEmpty())
                ld.insert(QStringLiteral("url"), e.url);
            ld.insert(QStringLiteral("organizer"), QJsonObject{{QStringLiteral("@type"), QStringLiteral("Person")},
                                                                {QStringLiteral("name"), organizer},
                                                                {QStringLiteral("email"), e.organizer.addr}});
            m.jsonLd = QJsonDocument(ld).toJson(QJsonDocument::Compact);
        }
    }
    m.text = lines.join(u'\n');
    m.html = html.join(QString());
    return m;
}

std::optional<Calendar::Invitation> Calendar::invitation(MailContext &ctx, const MessageDetail &m)
{
    if (m.calendar.isEmpty())
        return std::nullopt;
    const auto doc = ICal::parse(m.calendar.toUtf8(), m.calMethod.toLatin1());
    if (!doc || doc->events.isEmpty())
        return std::nullopt;
    Invitation inv;
    inv.method = doc->method;
    inv.event = doc->events.first();
    for (const ICal::Event &e : doc->events) {
        if (!m.calUid.isEmpty() && e.uid == m.calUid && !e.recurrenceId.isValid())
            inv.event = e;
    }
    const QString self = selfAddr(ctx);
    const QString from = mime::normalizeAddr(m.from.addr);
    inv.organizerIsSender = !inv.event.organizer.addr.isEmpty() && inv.event.organizer.addr == from;
    inv.organizerIsSelf = m.direction == Direction::Outgoing || inv.event.organizer.addr == self;
    if (const auto stored = get(ctx, inv.event.uid)) {
        inv.inCalendar = true;
        inv.cancelled = stored->status == ICal::Status::Cancelled;
        inv.outdated = stored->sequence > inv.event.sequence;
        inv.ourStatus = ourStatus(ctx, *stored);
        inv.attendees = organizedByUs(ctx, *stored) ? stored->attendees : inv.event.attendees;
    } else {
        inv.attendees = inv.event.attendees;
    }
    if (doc->method == ICal::Method::Reply) {
        for (const ICal::Attendee &a : inv.event.attendees) {
            if (a.addr == from) {
                inv.replyFrom = a.name.isEmpty() ? a.addr : a.name;
                inv.replyStatus = a.partstat;
            }
        }
    }
    return inv;
}

bool Calendar::actionable(MailContext &ctx, const MessageDetail &m)
{
    if (m.direction != Direction::Incoming || m.held || m.trashed)
        return false;
    const auto inv = invitation(ctx, m);
    return inv && inv->method == ICal::Method::Request && inv->organizerIsSender && !inv->organizerIsSelf
        && Contacts::isTrusted(ctx, m.from.addr);
}

Calendar::SendResult Calendar::save(MailContext &ctx, Event e, bool notify)
{
    SendResult r;
    const QString self = selfAddr(ctx);
    if (e.uid.isEmpty())
        e.uid = newUid(ctx);
    e = cleaned(e);
    QList<ICal::Attendee> attendees;
    for (const ICal::Attendee &a : e.attendees) {
        if (a.addr != self)
            attendees.append(a);
    }
    e.attendees = attendees;
    if (!e.attendees.isEmpty() && e.organizer.addr.isEmpty()) {
        e.organizer.addr = self;
        e.organizer.name = ctx.config.get(cfg::DisplayName).trimmed();
    }
    const auto before = get(ctx, e.uid);
    const bool ours = organizedByUs(ctx, e);
    const bool sentBefore = before && organizedByUs(ctx, *before) && !before->attendees.isEmpty();
    if (before) {
        // What attendees answered is theirs to say, not this edit's.
        for (ICal::Attendee &a : e.attendees) {
            for (const ICal::Attendee &b : before->attendees) {
                if (b.addr == a.addr) {
                    a.partstat = b.partstat;
                    if (const auto at = before->replied.constFind(a.addr); at != before->replied.constEnd())
                        e.replied.insert(a.addr, *at);
                }
            }
        }
        e.sequence = qMax(e.sequence, before->sequence);
        if (e.sourceMid.isEmpty())
            e.sourceMid = before->sourceMid;
    }
    if (ours && !e.attendees.isEmpty() && sentBefore && notify)
        ++e.sequence;
    e.stamp = QDateTime::currentDateTimeUtc();
    r.uid = saveLocal(ctx, e);
    if (!notify || !ours)
        return r;
    const Event saved = *get(ctx, r.uid);
    try {
        if (!saved.attendees.isEmpty()) {
            const InviteMail m = inviteMail(ctx, saved, ICal::Method::Request);
            r.queued.append(Compose::queue(ctx, draftFor(m, recipientsOf(saved.attendees, self))));
        }
        if (before) {
            // Anyone taken off the list hears that their copy is cancelled.
            Event gone = saved;
            gone.attendees.clear();
            for (const ICal::Attendee &b : before->attendees) {
                bool still = false;
                for (const ICal::Attendee &a : saved.attendees)
                    still = still || a.addr == b.addr;
                if (!still && b.addr != self)
                    gone.attendees.append(b);
            }
            if (!gone.attendees.isEmpty() && sentBefore) {
                const InviteMail m = inviteMail(ctx, gone, ICal::Method::Cancel);
                r.queued.append(Compose::queue(ctx, draftFor(m, recipientsOf(gone.attendees, self))));
            }
        }
    } catch (const ComposeError &err) {
        r.error = err.message();
        qCWarning(lcMail) << "invitation for" << r.uid << "not sent:" << r.error;
    }
    return r;
}

Calendar::SendResult Calendar::cancel(MailContext &ctx, const QString &uid, bool notify)
{
    SendResult r;
    auto e = get(ctx, uid);
    if (!e)
        return r;
    r.uid = uid;
    const bool ours = organizedByUs(ctx, *e);
    const bool tell = notify && ours && !e->attendees.isEmpty();
    e->status = ICal::Status::Cancelled;
    if (tell)
        ++e->sequence;
    e->stamp = QDateTime::currentDateTimeUtc();
    saveLocal(ctx, *e);
    if (!tell)
        return r;
    try {
        const InviteMail m = inviteMail(ctx, *e, ICal::Method::Cancel);
        r.queued.append(Compose::queue(ctx, draftFor(m, recipientsOf(e->attendees, selfAddr(ctx)))));
    } catch (const ComposeError &err) {
        r.error = err.message();
        qCWarning(lcMail) << "cancellation of" << uid << "not sent:" << r.error;
    }
    return r;
}

namespace {

// The invitation's event as ours, with our own row in it.
Calendar::Event adopt(MailContext &ctx, const Calendar::Invitation &inv, const QString &messageId,
                      const std::optional<Calendar::Event> &stored)
{
    Calendar::Event e;
    if (stored)
        e = *stored;
    static_cast<ICal::Event &>(e) = inv.event;
    e.sourceMid = messageId;
    if (stored) {
        e.id = stored->id;
        e.replied = stored->replied;
        if (!e.reminderMinutes)
            e.reminderMinutes = stored->reminderMinutes;
        // Answers the organizer told us about stay unless we heard newer ones.
        for (ICal::Attendee &a : e.attendees) {
            for (const ICal::Attendee &s : stored->attendees) {
                if (s.addr == a.addr && stored->replied.contains(a.addr))
                    a.partstat = s.partstat;
            }
        }
    }
    if (!e.reminderMinutes) {
        const int d = ctx.config.getInt(cfg::CalendarReminder);
        if (d >= 0)
            e.reminderMinutes = d;
    }
    return e;
}

} // namespace

Calendar::SendResult Calendar::rsvp(MailContext &ctx, qint64 msgId, ICal::PartStat answer)
{
    SendResult r;
    const auto d = Search::detail(ctx, msgId);
    if (!d || !actionable(ctx, *d)) {
        r.error = tr("This invitation cannot be answered.");
        return r;
    }
    const auto inv = invitation(ctx, *d);
    if (inv->outdated) {
        r.error = tr("A newer version of this invitation is in your calendar.");
        return r;
    }
    const QString self = selfAddr(ctx);
    Event e = adopt(ctx, *inv, d->messageId, get(ctx, inv->event.uid));
    bool found = false;
    for (ICal::Attendee &a : e.attendees) {
        if (a.addr == self) {
            a.partstat = answer;
            found = true;
        }
    }
    if (!found)
        e.attendees.append({self, displayName(ctx), answer, true});
    e.replied.insert(self, QDateTime::currentDateTimeUtc());
    r.uid = saveLocal(ctx, e);
    const Event saved = *get(ctx, r.uid);
    try {
        Draft reply = draftFor(inviteMail(ctx, saved, ICal::Method::Reply, answer), {{saved.organizer.name, saved.organizer.addr}});
        reply.inReplyToId = msgId;
        // An answer to an encrypted invitation never goes in cleartext (as
        // read receipts, ADR 0022).
        reply.encryption = d->encrypted ? SendEncryption::Required : SendEncryption::Auto;
        r.queued.append(Compose::queue(ctx, reply));
    } catch (const ComposeError &err) {
        r.error = err.message();
        qCWarning(lcMail) << "answer to" << saved.organizer.addr << "not sent:" << r.error;
    }
    return r;
}

Calendar::SendResult Calendar::addFromMessage(MailContext &ctx, qint64 msgId)
{
    SendResult r;
    const auto d = Search::detail(ctx, msgId);
    if (!d || d->direction != Direction::Incoming || d->held || d->trashed) {
        r.error = tr("This invitation cannot be added.");
        return r;
    }
    const auto inv = invitation(ctx, *d);
    if (!inv || inv->method == ICal::Method::Reply || inv->outdated) {
        r.error = tr("This invitation cannot be added.");
        return r;
    }
    Event e = adopt(ctx, *inv, d->messageId, get(ctx, inv->event.uid));
    if (inv->method == ICal::Method::Cancel)
        e.status = ICal::Status::Cancelled;
    // Someone we do not trust, or whose message does not name them as the
    // organizer, gets no answer and no say later: the facts, as a note of our
    // own, with the sender remembered as the organizer only when it is them.
    if (!inv->organizerIsSender || !Contacts::isTrusted(ctx, d->from.addr)) {
        e.attendees.clear();
        e.organizer = {};
    }
    r.uid = saveLocal(ctx, e);
    return r;
}

void Calendar::receive(MailContext &ctx, qint64 msgId, const ICal::Document &doc, const QString &fromAddr,
                       const QDateTime &date, bool fromSelf)
{
    Q_UNUSED(msgId);
    if (fromSelf)
        return; // our own invitation: the calendar already has it
    const QString from = mime::normalizeAddr(fromAddr);
    for (const ICal::Event &ev : doc.events) {
        if (ev.uid.isEmpty() || ev.recurrenceId.isValid())
            continue;
        const auto stored = get(ctx, ev.uid);
        if (!stored)
            continue; // nothing enters the calendar on arrival (ADR 0025)
        if (doc.method == ICal::Method::Reply) {
            if (organizedByUs(ctx, *stored))
                rederive(ctx, ev.uid);
            continue;
        }
        if (doc.method != ICal::Method::Request && doc.method != ICal::Method::Cancel)
            continue;
        // Only the organizer the person took the event from, writing as
        // themselves, may change or cancel it; and never with older news.
        if (stored->organizer.addr.isEmpty() || stored->organizer.addr != from || ev.organizer.addr != from)
            continue;
        if (!Contacts::isTrusted(ctx, from))
            continue;
        if (ev.sequence < stored->sequence)
            continue;
        Event e = *stored;
        if (doc.method == ICal::Method::Cancel) {
            e.status = ICal::Status::Cancelled;
            e.sequence = ev.sequence;
        } else {
            const int before = e.sequence;
            const QString self = selfAddr(ctx);
            const auto mine = ourStatus(ctx, *stored);
            static_cast<ICal::Event &>(e) = ev;
            e.reminderMinutes = stored->reminderMinutes ? stored->reminderMinutes : ev.reminderMinutes;
            e.tzid = ev.tzid;
            bool present = false;
            for (ICal::Attendee &a : e.attendees) {
                if (a.addr != self)
                    continue;
                present = true;
                // A changed meeting asks again; the same version keeps our answer.
                a.partstat = (ev.sequence == before && mine) ? *mine : ICal::PartStat::NeedsAction;
            }
            if (!present && mine)
                e.attendees.append({self, displayName(ctx), ev.sequence == before ? *mine : ICal::PartStat::NeedsAction, true});
        }
        e.stamp = ev.stamp.isValid() ? ev.stamp : date;
        writeEvent(ctx, e);
        qCInfo(lcMail) << "calendar:" << (doc.method == ICal::Method::Cancel ? "cancelled" : "updated") << ev.uid
                       << "from" << from;
    }
}

} // namespace e3::mail
