// SPDX-License-Identifier: MPL-2.0
#include "mail/ICal.h"
#include "mime/Part.h"

#include <QtTest>

using namespace e3;
using mail::ICal;

namespace {

QByteArray sample(const char *name)
{
    QFile f(QStringLiteral(E3_TEST_DATA "/ical/%1").arg(QLatin1String(name)));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// The text/calendar part of a message, wherever it sits.
const mime::Part *calendarPart(const mime::Part &p)
{
    if (p.mimeType == "text/calendar")
        return &p;
    for (const mime::Part &c : p.children) {
        if (const mime::Part *found = calendarPart(c))
            return found;
    }
    return nullptr;
}

QDateTime utc(int y, int mo, int d, int h, int mi)
{
    return QDateTime(QDate(y, mo, d), QTime(h, mi), QTimeZone::UTC);
}

} // namespace

class TestICal : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void gmailRequest()
    {
        const mime::Part msg = mime::parse(sample("gmail-request.eml"));
        const mime::Part *part = calendarPart(msg);
        QVERIFY(part);
        QCOMPARE(part->typeParams.value("method"), QStringLiteral("REQUEST"));
        const auto doc = ICal::parse(part->body);
        QVERIFY(doc);
        QCOMPARE(doc->method, ICal::Method::Request);
        QCOMPARE(doc->events.size(), 1);
        const ICal::Event &e = doc->events[0];
        QCOMPARE(e.uid, QStringLiteral("7k2p9q3rm1v8s4t6u0w2x5y7z9@google.com"));
        QCOMPARE(e.summary, QStringLiteral("Design review"));
        QCOMPARE(e.start, utc(2026, 11, 10, 9, 0));
        QCOMPARE(e.end, utc(2026, 11, 10, 10, 0));
        QVERIFY(!e.allDay);
        QVERIFY(e.tzid.isEmpty());
        QCOMPARE(e.location, QStringLiteral("Room 4, second floor"));
        QVERIFY(e.description.startsWith(QStringLiteral("Let's go over the sidebar mockups.")));
        QCOMPARE(e.organizer.addr, QStringLiteral("mira@example.org"));
        QCOMPARE(e.organizer.name, QStringLiteral("Mira Dorn"));
        QCOMPARE(e.attendees.size(), 2);
        QCOMPARE(e.attendees[0].addr, QStringLiteral("ada@example.org"));
        QCOMPARE(e.attendees[0].partstat, ICal::PartStat::NeedsAction);
        QVERIFY(e.attendees[0].rsvp);
        QCOMPARE(e.attendees[1].partstat, ICal::PartStat::Accepted);
        QCOMPARE(e.sequence, 0);
        QCOMPARE(e.status, ICal::Status::Confirmed);
        QCOMPARE(e.stamp, utc(2026, 11, 3, 8, 12).addSecs(44));
        QVERIFY(!e.reminderMinutes);
    }

    void thunderbirdRequest()
    {
        const mime::Part msg = mime::parse(sample("thunderbird-request.eml"));
        const mime::Part *part = calendarPart(msg);
        QVERIFY(part);
        const auto doc = ICal::parse(part->body);
        QVERIFY(doc);
        QCOMPARE(doc->method, ICal::Method::Request);
        const ICal::Event &e = doc->events.value(0);
        QCOMPARE(e.summary, QStringLiteral("Weekly standup"));
        QCOMPARE(e.tzid, QStringLiteral("Europe/Berlin"));
        QCOMPARE(e.start, utc(2026, 11, 9, 8, 30)); // CET
        QCOMPARE(e.end, utc(2026, 11, 9, 8, 45));
        QCOMPARE(e.rrule, QStringLiteral("FREQ=WEEKLY;BYDAY=MO"));
        QVERIFY(e.rule());
        QCOMPARE(e.rule()->freq, ICal::Rule::Freq::Weekly);
        QCOMPARE(e.rule()->byDay, QList<Qt::DayOfWeek>{Qt::Monday});
        QCOMPARE(e.exdates.size(), 1);
        QCOMPARE(e.exdates[0], utc(2026, 12, 21, 8, 30));
        QCOMPARE(e.organizer.name, QStringLiteral("Tomas Reyes"));
        QCOMPARE(e.attendees.size(), 2);
        QCOMPARE(e.description, QStringLiteral("Fifteen minutes, cameras optional."));
        QCOMPARE(e.reminderMinutes, std::optional<int>(10));
        // A folded attendee line was unfolded across the dot.
        QCOMPARE(e.attendees[1].addr, QStringLiteral("tomas@example.net"));
    }

    void outlookRequest()
    {
        const auto doc = ICal::parse(sample("outlook-request.ics"));
        QVERIFY(doc);
        QCOMPARE(doc->method, ICal::Method::Request);
        const ICal::Event &e = doc->events.value(0);
        QCOMPARE(e.summary, QStringLiteral("Budget meeting"));
        // Windows zone names are mapped to IANA ones.
        QCOMPARE(e.tzid, QStringLiteral("Europe/Berlin"));
        QCOMPARE(e.start, utc(2026, 11, 12, 13, 0));
        QCOMPARE(e.reminderMinutes, std::optional<int>(15));
        QCOMPARE(e.description, QStringLiteral("Quarterly numbers."));
        QCOMPARE(e.organizer.addr, QStringLiteral("dana@example.com"));
        QVERIFY(e.uid.startsWith(QStringLiteral("040000008200E00074C5B7101A82E008")));
    }

    void replyAndCancel()
    {
        const auto reply = ICal::parse(sample("reply.ics"));
        QVERIFY(reply);
        QCOMPARE(reply->method, ICal::Method::Reply);
        QCOMPARE(reply->events.value(0).attendees.size(), 1);
        QCOMPARE(reply->events.value(0).attendees.value(0).addr, QStringLiteral("mira@example.org"));
        QCOMPARE(reply->events.value(0).attendees.value(0).partstat, ICal::PartStat::Accepted);
        QCOMPARE(reply->events.value(0).stamp, utc(2026, 11, 3, 10, 15));

        const auto cancel = ICal::parse(sample("cancel.ics"));
        QVERIFY(cancel);
        QCOMPARE(cancel->method, ICal::Method::Cancel);
        QCOMPARE(cancel->events.value(0).status, ICal::Status::Cancelled);
        QCOMPARE(cancel->events.value(0).sequence, 1);
        QCOMPARE(cancel->events.value(0).uid, QStringLiteral("4f1c3a2e-9b7d-4c5a-8e6f-0a1b2c3d4e5f"));
    }

    void methodHintAndAccept()
    {
        QByteArray ics = sample("reply.ics");
        ics.replace("METHOD:REPLY\r\n", "");
        QCOMPARE(ICal::parse(ics)->method, ICal::Method::None);
        QCOMPARE(ICal::parse(ics, "reply")->method, ICal::Method::Reply);
        QVERIFY(!ICal::accept(ics).isEmpty());
        QVERIFY(ICal::accept("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nEND:VCALENDAR\r\n").isEmpty()); // no events
        QVERIFY(ICal::accept("hello").isEmpty());
    }

    void recurrence()
    {
        const auto doc = ICal::parse(sample("recurring.ics"));
        QVERIFY(doc);
        QCOMPARE(doc->events.size(), 4);
        const QDateTime from = utc(2026, 3, 1, 0, 0);
        const QDateTime to = utc(2026, 6, 1, 0, 0);

        // Every other Monday and Wednesday, six times, one left out, across
        // the DST change of 29 March: the wall clock stays at 09:00.
        const auto biweekly = ICal::occurrences(doc->events[0], from, to);
        QCOMPARE(biweekly.size(), 5);
        QCOMPARE(biweekly[0], utc(2026, 3, 16, 8, 0));
        QCOMPARE(biweekly[1], utc(2026, 3, 18, 8, 0));
        QCOMPARE(biweekly[2], utc(2026, 3, 30, 7, 0)); // CEST
        QCOMPARE(biweekly[3], utc(2026, 4, 13, 7, 0)); // 1 April is an EXDATE
        QCOMPARE(biweekly[4], utc(2026, 4, 15, 7, 0));

        // Three whole days, exclusive end; found when any of them is asked about.
        const ICal::Event &trip = doc->events[1];
        QVERIFY(trip.allDay);
        QCOMPARE(trip.start, utc(2026, 4, 20, 0, 0));
        QCOMPARE(trip.end, utc(2026, 4, 23, 0, 0));
        QCOMPARE(ICal::occurrences(trip, utc(2026, 4, 22, 0, 0), utc(2026, 4, 23, 0, 0)).size(), 1);
        QCOMPARE(ICal::occurrences(trip, utc(2026, 4, 23, 0, 0), utc(2026, 4, 24, 0, 0)).size(), 0);

        // Monthly on the 31st: months without one are skipped; UNTIL is inclusive.
        const auto monthly = ICal::occurrences(doc->events[2], utc(2026, 1, 1, 0, 0), utc(2027, 1, 1, 0, 0));
        QCOMPARE(monthly.size(), 4);
        QCOMPARE(monthly[1], utc(2026, 3, 31, 12, 0));
        QCOMPARE(monthly[3], utc(2026, 7, 31, 12, 0));

        // That event is given in UTC, so it repeats at noon UTC whatever the
        // reader's own clock does in March (ctest runs this suite again in
        // Los Angeles). A floating one is the reader's: it keeps the zone it
        // was read in, and its clock time there.
        QVERIFY(doc->events[2].tzid.isEmpty());
        const auto floating = ICal::parse("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:f\r\nDTSTART:20260301T080000\r\n"
                                          "RRULE:FREQ=DAILY;COUNT=20\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n");
        QVERIFY(floating);
        const QTimeZone here = QTimeZone::systemTimeZone();
        QCOMPARE(floating->events[0].tzid, QString::fromLatin1(here.id()));
        const auto mornings = ICal::occurrences(floating->events[0], utc(2026, 2, 28, 0, 0), utc(2026, 4, 1, 0, 0));
        QCOMPARE(mornings.size(), 20);
        for (int i = 0; i < mornings.size(); ++i)
            QCOMPARE(mornings[i], QDateTime(QDate(2026, 3, 1).addDays(i), QTime(8, 0), here).toUTC());

        // Daily in New York across its DST change on 8 March.
        const auto daily = ICal::occurrences(doc->events[3], from, utc(2026, 3, 11, 0, 0));
        QCOMPARE(daily.size(), 10);
        QCOMPARE(daily[0], utc(2026, 3, 1, 13, 0));
        QCOMPARE(daily[9], utc(2026, 3, 10, 12, 0));
        // Without an end the series is capped: a thousand instances, two years.
        const auto endless = ICal::occurrences(doc->events[3], from, from.addYears(10));
        QCOMPARE(endless.size(), 731);
        QVERIFY(endless.last() < from.addYears(2));
        QCOMPARE(ICal::occurrences(doc->events[3], from, from.addYears(10), 20).size(), 20);
        // A window in the middle of a long series finds its instances.
        const auto later = ICal::occurrences(doc->events[3], utc(2027, 6, 1, 0, 0), utc(2027, 6, 3, 0, 0));
        QCOMPARE(later.size(), 2);
    }

    void rules()
    {
        auto r = ICal::parseRule(QStringLiteral("FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE;COUNT=6"));
        QVERIFY(r);
        QCOMPARE(r->interval, 2);
        QCOMPARE(r->count, 6);
        QCOMPARE(ICal::writeRule(*r), QStringLiteral("FREQ=WEEKLY;INTERVAL=2;COUNT=6;BYDAY=MO,WE"));
        r = ICal::parseRule(QStringLiteral("FREQ=YEARLY;UNTIL=20301231"));
        QVERIFY(r);
        QCOMPARE(r->until.date(), QDate(2030, 12, 31));
        QVERIFY(!ICal::parseRule(QStringLiteral("FREQ=MONTHLY;BYMONTHDAY=1")));
        QVERIFY(!ICal::parseRule(QStringLiteral("FREQ=MONTHLY;BYDAY=2MO")));
        QVERIFY(!ICal::parseRule(QStringLiteral("FREQ=HOURLY")));
        QVERIFY(!ICal::parseRule(QStringLiteral("INTERVAL=2")));
        QVERIFY(!ICal::parseRule(QString()));
    }

    void writeRoundTrip()
    {
        ICal::Event e;
        e.uid = QStringLiteral("abc@e3mail");
        e.summary = QStringLiteral("Planning; the long one, with naïve ünïcode and a line that goes well past "
                                   "seventy-five octets so that it folds");
        e.description = QStringLiteral("First line\nSecond, with a comma; and a semicolon\\backslash");
        e.location = QStringLiteral("Room 1");
        e.url = QStringLiteral("https://example.org/meet");
        e.start = utc(2026, 11, 10, 9, 0);
        e.end = utc(2026, 11, 10, 10, 30);
        e.tzid = QStringLiteral("Europe/Berlin");
        e.rrule = QStringLiteral("FREQ=WEEKLY;BYDAY=TU;COUNT=4");
        e.exdates = {utc(2026, 11, 17, 9, 0)};
        e.organizer = {QStringLiteral("ada@example.org"), QStringLiteral("Ada Lovelace"), ICal::PartStat::Accepted, false};
        e.attendees = {{QStringLiteral("mira@example.org"), QStringLiteral("Mira Dorn"), ICal::PartStat::NeedsAction, true},
                       {QStringLiteral("tomas@example.net"), QString(), ICal::PartStat::Accepted, true}};
        e.sequence = 2;
        e.status = ICal::Status::Tentative;
        e.reminderMinutes = 15;
        e.stamp = utc(2026, 11, 3, 8, 0);
        const QByteArray ics = ICal::write({ICal::Method::Request, {e}});
        QVERIFY(ics.startsWith("BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"));
        QVERIFY(ics.contains("METHOD:REQUEST\r\n"));
        // A recurring timed event travels in its zone, with the zone defined.
        QVERIFY(ics.contains("BEGIN:VTIMEZONE\r\nTZID:Europe/Berlin\r\n"));
        QVERIFY(ics.contains("DTSTART;TZID=Europe/Berlin:20261110T100000\r\n"));
        QVERIFY(ics.contains("EXDATE;TZID=Europe/Berlin:20261117T100000\r\n"));
        QVERIFY(ics.contains("TZOFFSETFROM:+0200\r\nTZOFFSETTO:+0100\r\n"));
        QByteArray unfolded = ics;
        unfolded.replace("\r\n ", "");
        QVERIFY(unfolded.contains("ATTENDEE;CN=\"Mira Dorn\";ROLE=REQ-PARTICIPANT;PARTSTAT=NEEDS-ACTION;RSVP=TRUE:mailto:mira@example.org\r\n"));
        QVERIFY(ics.contains("TRIGGER:-PT15M\r\n"));
        QVERIFY(ics.contains("STATUS:TENTATIVE\r\n"));
        for (const QByteArray &line : ics.split('\n'))
            QVERIFY2(line.size() <= 76, line.constData());

        const auto back = ICal::parse(ics);
        QVERIFY(back);
        QCOMPARE(back->method, ICal::Method::Request);
        const ICal::Event &b = back->events.value(0);
        QCOMPARE(b.uid, e.uid);
        QCOMPARE(b.summary, e.summary);
        QCOMPARE(b.description, e.description);
        QCOMPARE(b.location, e.location);
        QCOMPARE(b.url, e.url);
        QCOMPARE(b.start, e.start);
        QCOMPARE(b.end, e.end);
        QCOMPARE(b.tzid, e.tzid);
        QCOMPARE(b.rrule, e.rrule);
        QCOMPARE(b.exdates, e.exdates);
        QCOMPARE(b.organizer.addr, e.organizer.addr);
        QCOMPARE(b.organizer.name, e.organizer.name);
        QCOMPARE(b.attendees, e.attendees);
        QCOMPARE(b.sequence, 2);
        QCOMPARE(b.status, ICal::Status::Tentative);
        QCOMPARE(b.reminderMinutes, std::optional<int>(15));
        QCOMPARE(b.stamp, e.stamp);
        QCOMPARE(ICal::write(*back), ics);

        // A single timed event is written in UTC, without a zone.
        ICal::Event single = e;
        single.rrule.clear();
        single.exdates.clear();
        const QByteArray one = ICal::write({ICal::Method::None, {single}});
        QVERIFY(!one.contains("VTIMEZONE"));
        QVERIFY(one.contains("DTSTART:20261110T090000Z\r\n"));
        QVERIFY(!one.contains("METHOD:"));
        // All-day: dates, end exclusive.
        ICal::Event day;
        day.uid = QStringLiteral("d");
        day.allDay = true;
        day.start = utc(2026, 12, 24, 0, 0);
        day.end = utc(2026, 12, 27, 0, 0);
        day.summary = QStringLiteral("Away");
        const QByteArray days = ICal::write({ICal::Method::None, {day}});
        QVERIFY(days.contains("DTSTART;VALUE=DATE:20261224\r\nDTEND;VALUE=DATE:20261227\r\n"));
        const auto dayBack = ICal::parse(days);
        QVERIFY(dayBack && dayBack->events.value(0).allDay);
        QCOMPARE(dayBack->events.value(0).end, day.end);
    }

    void hostileInput()
    {
        QVERIFY(!ICal::parse(QByteArray(ICal::kMaxBytes + 1, 'x')));
        QVERIFY(!ICal::parse("not a calendar"));
        QVERIFY(!ICal::parse(""));
        // Unbalanced components and unknown ones are survived.
        QByteArray odd = "BEGIN:VCALENDAR\r\nBEGIN:VTODO\r\nSUMMARY:x\r\nEND:VTODO\r\nBEGIN:VEVENT\r\nUID:u\r\n"
                         "DTSTART:20260101T100000Z\r\nSUMMARY:ok\r\nBEGIN:VFREEBUSY\r\nEND:VEVENT\r\n";
        auto doc = ICal::parse(odd);
        QVERIFY(doc);
        QCOMPARE(doc->events.size(), 0); // the event never ended; nothing half-read is kept
        // Markup and bidi controls become plain text; long values are cut.
        QByteArray tricky = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:u\r\nDTSTART:20260101T100000Z\r\n"
                            "SUMMARY:<b>bold</b> \xE2\x80\xAE reversed\r\nLOCATION:" + QByteArray(1000, 'L') + "\r\n"
                            "DESCRIPTION:" + QByteArray(10000, 'D') + "\r\nURL:javascript:alert(1)\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
        doc = ICal::parse(tricky);
        QVERIFY(doc);
        QCOMPARE(doc->events.size(), 1);
        QCOMPARE(doc->events[0].summary, QStringLiteral("<b>bold</b> reversed"));
        QCOMPARE(doc->events[0].location.size(), ICal::kMaxShort);
        QCOMPARE(doc->events[0].description.size(), ICal::kMaxText);
        QVERIFY(doc->events[0].url.isEmpty());
        QCOMPARE(doc->events[0].end, doc->events[0].start); // no DTEND, no DURATION: a point in time
        // Too many attendees are cut; duplicates are one.
        QByteArray crowd = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:u\r\nDTSTART:20260101T100000Z\r\n";
        for (int i = 0; i < 300; ++i)
            crowd += "ATTENDEE:mailto:p" + QByteArray::number(i % 250) + "@example.org\r\n";
        crowd += "END:VEVENT\r\nEND:VCALENDAR\r\n";
        doc = ICal::parse(crowd);
        QCOMPARE(doc->events.value(0).attendees.size(), ICal::kMaxAttendees);
        // An unknown zone takes the offset its VTIMEZONE declares; without one, the reader's.
        QByteArray zone = "BEGIN:VCALENDAR\r\nBEGIN:VTIMEZONE\r\nTZID:Mars/Olympus\r\nBEGIN:STANDARD\r\n"
                          "TZOFFSETFROM:+0300\r\nTZOFFSETTO:+0300\r\nDTSTART:19700101T000000\r\nEND:STANDARD\r\nEND:VTIMEZONE\r\n"
                          "BEGIN:VEVENT\r\nUID:u\r\nDTSTART;TZID=Mars/Olympus:20260101T100000\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
        doc = ICal::parse(zone);
        QCOMPARE(doc->events.value(0).start, utc(2026, 1, 1, 7, 0));
        QVERIFY(doc->events.value(0).tzid.isEmpty());
        // DURATION gives the end.
        QByteArray dur = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:u\r\nDTSTART:20260101T100000Z\r\nDURATION:PT1H30M\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
        QCOMPARE(ICal::parse(dur)->events.value(0).end, utc(2026, 1, 1, 11, 30));
        // A reminder a day ahead; one after the start is none.
        QByteArray alarm = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:u\r\nDTSTART:20260101T100000Z\r\n"
                           "BEGIN:VALARM\r\nTRIGGER:PT5M\r\nACTION:DISPLAY\r\nEND:VALARM\r\n"
                           "BEGIN:VALARM\r\nTRIGGER:-P1D\r\nACTION:DISPLAY\r\nEND:VALARM\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
        QCOMPARE(ICal::parse(alarm)->events.value(0).reminderMinutes, std::optional<int>(1440));
    }

    void escapingAndFolding()
    {
        QCOMPARE(ICal::escapeText(QStringLiteral("a;b,c\\d\ne")), QByteArray("a\\;b\\,c\\\\d\\ne"));
        const QByteArray folded = ICal::fold("SUMMARY:" + QByteArray(100, 'x'));
        QCOMPARE(folded.indexOf("\r\n "), 75);
        QVERIFY(folded.endsWith("\r\n"));
        // Never inside a UTF-8 sequence.
        QByteArray umlauts = "SUMMARY:";
        for (int i = 0; i < 60; ++i)
            umlauts += "\xC3\xBC";
        for (const QByteArray &line : ICal::fold(umlauts).split('\n')) {
            const QByteArray l = line.trimmed();
            QVERIFY(QString::fromUtf8(l).toUtf8() == l); // round trips: no broken sequence
        }
    }
};

QTEST_GUILESS_MAIN(TestICal)
#include "tst_ical.moc"
