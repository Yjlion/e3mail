# 0025 — A local calendar, with meetings arranged by mail

**Status:** Accepted — 2026-11-04

## Context
A classic mail client without a calendar sends its users to a web calendar
for every meeting, and those calendars live on someone else's server.
e3mail's premise is that the mailbox is local and the server is a spool
(ADR 0006); a calendar fits the same way. Meetings between people are
arranged by iTIP over iCalendar (RFC 5546, RFC 5545): Gmail, Outlook, Apple
Mail and Thunderbird all send and read a `text/calendar` part. Until now
e3mail dropped that part inside `multipart/alternative` and dumped it as
text elsewhere.

The person asked for: a calendar local to the device, in the spirit of an
end-to-end encrypted client; no CalDAV and no groupware; invitations sent
and received by mail; an update by mail when a meeting changes; and the
structured-mail work (ADR 0024) finished so that e3mail sends structured
data too.

## Decision
- **Local only.** Events live in each device's database (`events`,
  `event_attendees`, migration 8) and travel to the person's other devices
  through the op-log (ADR 0013 amendment of this date). There is no CalDAV,
  no free/busy, no shared calendar. Backups (ADR 0020) and pairing carry it.
- **Meetings by mail.** An invitation is ordinary mail through
  `Compose::queue`: it lands in Sent, is threaded, and follows the
  encryption policy (ADR 0004), so a meeting among people with keys is
  arranged in encrypted mail and the server never sees its title. The MIME
  shape is `multipart/alternative` of: the schema.org Event as
  `application/ld+json` with `Content-Purpose: Machine-readable` (first,
  since clients show the last part they understand), `text/plain`,
  `text/html`, and `text/calendar; method=…` last. No `invite.ics`
  attachment: Gmail's own is the same part again, and Ingest reads either.
- **Nothing enters the calendar on arrival.** A REQUEST is shown as a card;
  the event is added when the person answers (Accept, Tentative, Decline)
  or presses Add. Once added, later news about it is **derived** on every
  device from the mail itself, which every device fetches (ADR 0010):
  - the organizer's REQUEST with a `SEQUENCE` no lower than the stored one
    updates the facts and asks again (our status returns to needs-action
    when the sequence rose);
  - the organizer's CANCEL cancels it (the row stays, so a stale REQUEST
    cannot revive it);
  - an attendee's REPLY sets that attendee's status, newest `DTSTAMP`
    winning, with messages read in date order deciding ties.
  None of this is an op; only what the person does is recorded.
- **The sender is the authority, not the iCalendar text.** A REQUEST or
  CANCEL counts only when the message's From is the `ORGANIZER` the event
  was taken from, and that sender is trusted (ADR 0022). A REPLY counts
  only for the `ATTENDEE` whose address is the From, on an event we
  organize. A forwarded invitation shows its facts but offers no answer;
  it can be added as a note of one's own, with no organizer to answer to.
- **Who may answer.** Incoming, not held, not in Trash, from someone
  trusted, the organizer being the sender, and not our own. A signature is
  *not* required, unlike ADR 0024's cards: almost no real invitation is
  OpenPGP-signed, and a reply only tells someone the person already
  corresponds with that they read it. A stranger's invitation waits in
  Unverified, inert.
- **An answer to an encrypted invitation is sent only encrypted**, as read
  receipts are (ADR 0022); if the organizer's key is missing the status is
  saved and the person is told the answer did not go.
- **Scope.** Timed and all-day events; zones by TZID (IANA, Windows names,
  or the offset a VTIMEZONE declares); recurrence as daily, weekly with
  weekdays, monthly and yearly, with interval and count or until; single
  occurrences left out by EXDATE; one reminder per event. Occurrences
  expand in the event's zone (a weekly 09:00 stays 09:00 across DST) and
  stop at a thousand instances or two years. Other rules are kept as
  received, shown as text, and give the first occurrence only.
- **What is written.** UTC for single events; a recurring timed event
  carries its zone and a VTIMEZONE built from the zone's transitions.
  Lines fold at 75 octets.
- **Limits on what is read.** 256 KiB, 64 events, 200 attendees, 500
  exdates, 400 characters a value and 4000 for a description; markup,
  control and bidi characters dropped; only http and https links. The
  part is kept with the message as received and parsed again when shown.
- **Reminders** are per device, through the system notifier, only while
  e3mail runs; what was shown is remembered so a restart does not repeat
  it. A default (`calendar_reminder`, synced) applies to what is added.
- **Structured mail is now sent** (ADR 0024 amendment): the Event beside
  the invitation. Received cards' "Add to calendar" creates a local event.

## Consequences
- An invitation to a mixed list (some attendees without keys) goes in
  cleartext to all, as every message does under ADR 0004; the editor says
  so before Save.
- Two edits of one event on two devices: the later wins as a whole
  (ADR 0013). Attendee answers survive it, since each device derives them
  from the same mail.
- A spoofed From equal to the organizer's address can change or cancel an
  event the person already added, the same exposure as any unsigned mail;
  the message stays in the Inbox and its card shows the change.
- `Transaction` does not nest: the event is saved before its mail is
  queued, so a policy refusal leaves the event saved and reported, not
  rolled back.
- Declined invitations stay in the store, hidden from the views, so later
  updates and cancellations still match them.
- Interoperability is designed from the clients' documented and observed
  output, not verified against them here; see the handoff.

## Amendment — 2026-11-10: the week and the day as hours

The calendar had a month and an agenda. A week and a day are now drawn as
a grid of hours, and an event can be moved by dragging it.

- **The layout is decided in the core** (`mail/CalendarLayout`), from
  nothing but when things start and end, so its rules are tested without
  the interface:
  - An all-day event, and a timed one lasting 24 hours or more, is a bar
    above the hours. An all-day event's days are its dates as stored (UTC),
    not where its midnights fall on the reader's clock: the tenth is the
    tenth in Tokyo and in Los Angeles.
  - A timed event that crosses midnight is drawn on each day it touches;
    one ending at midnight ends that day.
  - A day is 24 rows of the reader's wall clock, also on the days that
    last 23 or 25 hours. An event sits at the time its clock shows; in the
    hour the clocks repeat, one that ends at an earlier clock time than it
    began is drawn as long as it lasts.
  - Events that overlap, directly or through others, share the day's
    width in columns; each takes the first column free when it starts. A
    block is never drawn shorter than 20 minutes, and that drawn length is
    what counts as overlapping, so short events never cover each other.
  - Bars take the first lane free on their first day, the longer first.
- **The week is the locale's week** (`QLocale::firstDayOfWeek`). A phone
  shows three days from the chosen day instead of seven. The hours in the
  margin and the title's span of days are written by CLDR's patterns
  through ICU (`LocalePatterns::hour`, `dateRange`).
- **The view chosen is kept per device** (`calendarMode` in `app.ini`),
  like the other things that are about a screen rather than about the
  account.
- **What dragging may change** (`Calendar::reschedule`): a timed event of
  the person's own that does not repeat. Someone else's meeting is theirs
  to move. One occurrence of a series cannot be changed alone (no
  RECURRENCE-ID edits yet), so a repeating event is not moved at all
  rather than moving every occurrence by surprise. All-day events are
  changed in the editor.
- **A dragged meeting asks before it mails.** It is `Calendar::save`, so a
  meeting with attendees raises its sequence and sends a new REQUEST; the
  person is asked first, with the editor's own question (Send, Save only,
  Back). Nothing is saved until they answer.
- **Dragging is the mouse's.** A finger on the grid scrolls it; on a phone
  an event is moved in its editor. Times snap to 15 minutes.
- **A click on an empty half hour** opens a new event there, an hour long.
