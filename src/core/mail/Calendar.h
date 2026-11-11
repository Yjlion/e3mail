// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "ICal.h"
#include "Types.h"

#include <QHash>
#include <QJsonObject>

#include <optional>

namespace e3::mail {

// The calendar (ADR 0025): events that live on the person's devices and
// nowhere else, arranged by mail. What the person does (creating, editing,
// answering, taking an event out) is recorded as ops; what arriving mail
// does to an event the person already has (an organizer's update or
// cancellation, an attendee's answer) is derived on every device from the
// message itself, which every device fetches.
class Calendar
{
public:
    struct Event : ICal::Event
    {
        qint64 id = 0;
        QString sourceMid;                 // the invitation it came from, if any
        QDateTime updatedAt;
        QHash<QString, QDateTime> replied; // attendee -> DTSTAMP of the newest answer seen
    };
    // One instance of an event: the event itself, with its own start and end
    // when it recurs.
    struct Occurrence
    {
        Event event;
        QDateTime start;
        QDateTime end;
    };

    // --- Reading ---
    // Instances overlapping [from, to), in time order. Cancelled events and
    // invitations we declined are left out.
    static QList<Occurrence> list(MailContext &ctx, const QDateTime &from, const QDateTime &to);
    static std::optional<Event> get(MailContext &ctx, const QString &uid);
    static std::optional<Event> byId(MailContext &ctx, qint64 id);
    // Our own standing in an event someone else organizes, if we are asked.
    static std::optional<ICal::PartStat> ourStatus(MailContext &ctx, const Event &e);
    static bool organizedByUs(MailContext &ctx, const Event &e);
    static QByteArray exportIcs(MailContext &ctx, const QString &uid);

    // --- The person's own changes: each opens a transaction and records its op ---
    // Creates or updates by uid (a new uid when empty); returns the uid.
    static QString saveLocal(MailContext &ctx, Event e);
    // Out of this calendar. Never mails anyone.
    static void removeLocal(MailContext &ctx, const QString &uid);

    // --- Invitations by mail ---
    struct SendResult
    {
        QString uid;
        QList<qint64> queued; // message ids
        QString error;        // the policy's refusal, when a mail could not go
    };
    // Saves, and when we organize it and it has attendees, raises the
    // sequence and sends REQUEST to every attendee and CANCEL to anyone taken
    // off the list (through Compose::queue, so the mail follows the
    // encryption policy and lands in Sent). The event is saved whether or not
    // the mail goes; `error` says when it did not.
    static SendResult save(MailContext &ctx, Event e, bool notify);
    // Moves an event to another time and keeps the rest of it, as dragging
    // it in the week does (ADR 0025, amendment 2026-11-10). Only a timed
    // event of our own that does not repeat: someone else's meeting is
    // theirs to move, and one occurrence of a series cannot be changed
    // alone. Refused, the result has no uid and nothing is changed.
    // Otherwise it is `save`: a meeting's attendees are asked again when
    // `notify`.
    static SendResult reschedule(MailContext &ctx, const QString &uid, const QDateTime &start, const QDateTime &end,
                                 bool notify);
    // May `reschedule` move it?
    static bool movable(MailContext &ctx, const Event &e);
    // Cancels: status cancelled, a CANCEL to every attendee when we organize
    // it and `notify`.
    static SendResult cancel(MailContext &ctx, const QString &uid, bool notify);
    // Answers the invitation a message carries: the event goes into the
    // calendar with our status, and a REPLY goes to the organizer.
    static SendResult rsvp(MailContext &ctx, qint64 msgId, ICal::PartStat answer);
    // Takes an invitation's event into the calendar without answering.
    static SendResult addFromMessage(MailContext &ctx, qint64 msgId);
    // A structured card's item (ADR 0024 amendment) as a local event.
    static QString addEvent(MailContext &ctx, const ICal::Event &e, const QString &sourceMid);

    // What a message's calendar part means now, for the card.
    struct Invitation
    {
        ICal::Method method = ICal::Method::None;
        ICal::Event event;         // as the message says
        bool organizerIsSender = false; // the ics ORGANIZER is the From
        bool organizerIsSelf = false;   // our own invitation
        bool inCalendar = false;
        bool outdated = false;          // a newer sequence is in the calendar
        bool cancelled = false;         // in the calendar, cancelled since
        std::optional<ICal::PartStat> ourStatus;  // from the calendar
        QList<ICal::Attendee> attendees;          // from the calendar when we organize it
        QString replyFrom;                        // METHOD:REPLY: who answered what
        ICal::PartStat replyStatus = ICal::PartStat::NeedsAction;
    };
    static std::optional<Invitation> invitation(MailContext &ctx, const MessageDetail &m);
    // May the person act on this invitation? Incoming, not held or trashed,
    // from someone trusted (ADR 0022), the organizer is the sender, and not
    // our own. A signature is not required (unlike ADR 0024's cards).
    static bool actionable(MailContext &ctx, const MessageDetail &m);

    // --- Ingest hook: inside its transaction, derived, never records ---
    static void receive(MailContext &ctx, qint64 msgId, const ICal::Document &doc, const QString &from,
                        const QDateTime &date, bool fromSelf);
    // Attendee answers from every REPLY stored for this uid, for an event
    // that arrived after them.
    static void rederive(MailContext &ctx, const QString &uid);

    // --- For ops from other devices: no transaction, no op ---
    static bool applyEvent(MailContext &ctx, const QJsonObject &payload);
    static void applyRemove(MailContext &ctx, const QString &uid);
    static QJsonObject payload(const Event &e);
    static Event fromPayload(const QJsonObject &p);

    // --- Reminders, per device ---
    // Occurrences whose reminder is due at `now` and not shown yet; marks them shown.
    static QList<Occurrence> dueReminders(MailContext &ctx, qint64 nowMs);
    // When the next reminder is due, if any within 32 days.
    static std::optional<qint64> nextReminderMs(MailContext &ctx, qint64 nowMs);

    // The mail for an iTIP method: subject, bodies and the parts.
    struct InviteMail
    {
        QString subject;
        QString text;
        QString html;
        QByteArray ics;
        QByteArray method;
        QByteArray jsonLd;
    };
    static InviteMail inviteMail(MailContext &ctx, const Event &e, ICal::Method method,
                                 std::optional<ICal::PartStat> answer = {});
    static QString describeRule(const QString &rrule);
    // "Tue 10 Nov 2026, 10:00–11:00" in the reader's zone and locale.
    static QString describeWhen(const ICal::Event &e, const QLocale &locale = QLocale());
};

} // namespace e3::mail
