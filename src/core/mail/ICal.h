// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QTimeZone>

#include <optional>

namespace e3::mail {

// iCalendar (RFC 5545) and the iTIP methods (RFC 5546) e3mail speaks: enough
// of it for invitations by mail (ADR 0025). Everything read is bounded and
// plain text; what is written is what other clients read: UTC times, or a
// zone with its VTIMEZONE where a recurrence needs one.
class ICal
{
public:
    static constexpr qsizetype kMaxBytes = 256 * 1024;
    static constexpr int kMaxEvents = 64;
    static constexpr int kMaxAttendees = 200;
    static constexpr int kMaxExdates = 500;
    static constexpr int kMaxText = 4000; // a description
    static constexpr int kMaxShort = 400; // everything else
    static constexpr int kMaxOccurrences = 1000;

    enum class Method { None, Publish, Request, Reply, Cancel };
    enum class PartStat { NeedsAction = 0, Accepted = 1, Tentative = 2, Declined = 3 };
    enum class Status { Confirmed = 0, Tentative = 1, Cancelled = 2 };

    struct Attendee
    {
        QString addr; // normalised
        QString name;
        PartStat partstat = PartStat::NeedsAction;
        bool rsvp = false;
        bool operator==(const Attendee &) const = default;
    };

    // The recurrence subset e3mail understands: a frequency, an interval, an
    // end by count or date, and weekdays for weekly rules.
    struct Rule
    {
        enum class Freq { Daily, Weekly, Monthly, Yearly };
        Freq freq = Freq::Weekly;
        int interval = 1;
        int count = 0;            // 0: not bounded by count
        QDateTime until;          // invalid: not bounded by date; inclusive, UTC
        QList<Qt::DayOfWeek> byDay; // weekly only; empty: the start's weekday
        bool operator==(const Rule &) const = default;
    };

    struct Event
    {
        QString uid;
        QString summary;
        QString description;
        QString location;
        QString url;
        QDateTime start; // UTC. All-day: the date at 00:00 UTC.
        QDateTime end;   // UTC, exclusive. All-day: midnight after the last day.
        bool allDay = false;
        QString tzid;    // IANA zone the times were given in (the reader's, for floating times), or empty (UTC)
        QString rrule;   // the RRULE value as received, or empty
        QList<QDateTime> exdates; // occurrence starts left out
        Attendee organizer;       // addr empty when there is none
        QList<Attendee> attendees;
        int sequence = 0;
        Status status = Status::Confirmed;
        std::optional<int> reminderMinutes; // the first VALARM before the start
        QDateTime stamp;                    // DTSTAMP
        QDateTime recurrenceId;             // set on a changed occurrence (not applied)

        // The rule, if the RRULE is one e3mail understands.
        std::optional<Rule> rule() const { return ICal::parseRule(rrule); }
        // How long one occurrence lasts.
        qint64 durationMs() const { return start.msecsTo(end); }
        // The zone recurrence follows: `tzid`, or UTC without one.
        QTimeZone zone() const;
    };

    struct Document
    {
        Method method = Method::None;
        QList<Event> events;
    };

    // Reads a calendar; nullopt when it is not one or is over the limits.
    // `methodHint` is the Content-Type's method parameter, used when the
    // calendar itself names none. Never throws.
    static std::optional<Document> parse(const QByteArray &ics, const QByteArray &methodHint = {});
    // The text to keep with a message: within the limits and readable, or empty.
    static QString accept(const QByteArray &ics, const QByteArray &methodHint = {});
    // A calendar as other clients read it: CRLF, folded at 75 octets, times in
    // UTC, except that a recurring timed event in a zone carries the zone and
    // its VTIMEZONE.
    static QByteArray write(const Document &doc);

    static std::optional<Rule> parseRule(const QString &rrule);
    static QString writeRule(const Rule &rule);

    // Occurrence starts that overlap [from, to), in time order, without the
    // exdates; at most `limit`, and never past two years after `from`.
    static QList<QDateTime> occurrences(const Event &e, const QDateTime &from, const QDateTime &to,
                                        int limit = kMaxOccurrences);

    static QByteArray methodName(Method m);
    static Method methodFrom(const QByteArray &name);
    static QByteArray partStatName(PartStat p);
    static PartStat partStatFrom(const QByteArray &name);

    // Shared with other writers (Structured::toIcs).
    static QByteArray escapeText(const QString &s);
    static QByteArray fold(const QByteArray &line);          // one content line, CRLF ended
    // "DTSTART;VALUE=DATE:20261103", "DTSTART:20261103T084000Z" or
    // "DTSTART;TZID=Europe/Berlin:20261103T094000", folded.
    static QByteArray timeLine(const QByteArray &name, const QDateTime &t, bool allDay, const QString &tzid = {});
    // Plain text, bounded, without control or bidi characters.
    static QString clean(const QString &s, int limit, bool keepNewlines);
};

} // namespace e3::mail
