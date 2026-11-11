// SPDX-License-Identifier: MPL-2.0
#include "FakeMailServer.h"
#include "engine/AccountManager.h"
#include "engine/MailWorker.h"
#include "mail/Calendar.h"
#include "mail/Compose.h"
#include "mail/Contacts.h"
#include "mail/ICal.h"
#include "mail/Ingest.h"
#include "mail/Organize.h"
#include "mail/Preferences.h"
#include "mail/Search.h"
#include "mail/Structured.h"
#include "mime/Builder.h"
#include "mime/Part.h"
#include "store/Config.h"
#include "store/Database.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace e3;
using mail::Calendar;
using mail::ICal;

namespace {

QByteArray sample(const char *name)
{
    QFile f(QStringLiteral(E3_TEST_DATA "/%1").arg(QLatin1String(name)));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QDateTime utc(int y, int mo, int d, int h, int mi)
{
    return QDateTime(QDate(y, mo, d), QTime(h, mi), QTimeZone::UTC);
}

} // namespace

// Invitations by mail (ADR 0025) between real accounts over the in-process
// mail system: organizer and attendees, answers, updates, cancellations,
// and who may act on what.
class TestCalendar : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<FakeMailServer> m_srv;
    std::unique_ptr<AccountManager> m_mgr;
    Account *m_alice = nullptr;
    Account *m_bob = nullptr;
    Account *m_carol = nullptr;
    Account *m_dana = nullptr;

    static QString alice() { return QStringLiteral("alice@x.test"); }
    static QString bob() { return QStringLiteral("bob@x.test"); }
    static QString carol() { return QStringLiteral("carol@x.test"); }
    static QString dana() { return QStringLiteral("dana@x.test"); }

    Account *makeAccount(const QString &addr)
    {
        m_srv->addUser(addr, QStringLiteral("pw"));
        Account *a = m_mgr->create();
        Account::Settings s;
        s.addr = addr;
        s.displayName = addr.section(u'@', 0, 0);
        s.protocol = QStringLiteral("imap");
        s.inHost = s.smtpHost = QStringLiteral("127.0.0.1");
        s.inPort = m_srv->imapPort();
        s.smtpPort = m_srv->smtpPort();
        s.inSecurity = s.smtpSecurity = QStringLiteral("plain");
        s.inPassword = QStringLiteral("pw");
        a->configure(s);
        return a;
    }

    static void sync(Account *a) { MailWorker::runOnce(a->dir(), a->credentials()); }

    // The newest message with this subject, in the Inbox or held.
    static qint64 find(Account *a, const QString &subject, const QString &tag = tag::Inbox)
    {
        for (const auto &m : mail::Search::list(a->ctx(), SearchQuery::forTag(tag))) {
            if (m.subject == subject)
                return m.id;
        }
        return 0;
    }

    static MessageDetail detail(Account *a, qint64 id) { return *mail::Search::detail(a->ctx(), id); }

    // Alice's event with Bob and Carol, sent; everyone fetches. Returns the uid.
    QString aliceInvites(const QString &summary = QStringLiteral("Design review"))
    {
        Calendar::Event e;
        e.summary = summary;
        e.description = QStringLiteral("Mockups, then lunch.");
        e.location = QStringLiteral("Room 4");
        e.start = utc(2026, 11, 10, 9, 0);
        e.end = utc(2026, 11, 10, 10, 0);
        e.attendees = {{bob(), QStringLiteral("Bob"), ICal::PartStat::NeedsAction, true},
                       {carol(), QStringLiteral("Carol"), ICal::PartStat::NeedsAction, true}};
        const Calendar::SendResult r = Calendar::save(m_alice->ctx(), e, true);
        if (!r.error.isEmpty())
            qWarning() << r.error;
        sync(m_alice);
        sync(m_bob);
        sync(m_carol);
        return r.uid;
    }

private Q_SLOTS:
    void init()
    {
        qputenv("E3MAIL_NO_KEYCHAIN", "1");
        m_dir = std::make_unique<QTemporaryDir>();
        m_srv = std::make_unique<FakeMailServer>();
        m_mgr = std::make_unique<AccountManager>(m_dir->path() + QStringLiteral("/main"));
        m_alice = makeAccount(alice());
        m_bob = makeAccount(bob());
        m_carol = makeAccount(carol());
        m_dana = makeAccount(dana());
        // Alice, Bob and Carol know each other; Dana is a stranger to all.
        for (Account *a : {m_alice, m_bob, m_carol}) {
            for (const QString &other : {alice(), bob(), carol()}) {
                if (other != a->ctx().config.get(cfg::Addr))
                    mail::Organize::accept(a->ctx(), other);
            }
        }
    }

    void cleanup()
    {
        m_mgr.reset();
        m_srv.reset();
        m_dir.reset();
    }

    void mimeShape()
    {
        // What we send: JSON-LD first, text, HTML, the calendar last; and what
        // we read back from it, and from Gmail's shape, lists no attachment
        // and shows no raw VCALENDAR.
        mime::OutgoingMail m;
        m.from = {QStringLiteral("Alice"), alice()};
        m.to = {{QString(), bob()}};
        m.subject = QStringLiteral("Invitation: x");
        m.date = utc(2026, 11, 3, 8, 0);
        m.messageId = QStringLiteral("m1@x.test");
        m.text = QStringLiteral("Alice invites you.");
        m.html = QStringLiteral("<p>Alice invites you.</p>");
        m.calendar = "BEGIN:VCALENDAR\r\nMETHOD:REQUEST\r\nBEGIN:VEVENT\r\nUID:u\r\nDTSTART:20261110T090000Z\r\n"
                     "SUMMARY:x\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
        m.calendarMethod = "REQUEST";
        m.structured = "{\"@type\":\"Event\",\"name\":\"x\",\"startDate\":\"2026-11-10T09:00:00Z\"}";
        const QByteArray raw = mime::Builder::build(m);
        const mime::Part p = mime::parse(raw);
        QCOMPARE(p.mimeType, QByteArray("multipart/alternative"));
        QCOMPARE(p.children.size(), 4);
        QCOMPARE(p.children[0].mimeType, QByteArray("application/ld+json"));
        QCOMPARE(p.children[0].headers.raw("Content-Purpose").trimmed(), QByteArray("Machine-readable"));
        QCOMPARE(p.children[3].mimeType, QByteArray("text/calendar"));
        QCOMPARE(p.children[3].typeParams.value("method"), QStringLiteral("REQUEST"));
        const mail::Ingest::Content c = mail::Ingest::extract(p);
        QCOMPARE(c.calendar, m.calendar);
        QCOMPARE(c.calendarMethod, QByteArray("REQUEST"));
        QCOMPARE(c.structured, m.structured);
        QCOMPARE(c.text, QStringLiteral("Alice invites you."));
        QVERIFY(c.html.contains(QStringLiteral("invites")));
        QVERIFY(c.attachments.isEmpty());

        const mail::Ingest::Content g = mail::Ingest::extract(mime::parse(sample("ical/gmail-request.eml")));
        QVERIFY(g.calendar.contains("BEGIN:VCALENDAR"));
        QCOMPARE(g.calendarMethod, QByteArray("REQUEST"));
        QVERIFY(g.attachments.isEmpty()); // invite.ics is the same thing again
        QVERIFY(!g.text.contains(QStringLiteral("VCALENDAR")));
        const mail::Ingest::Content t = mail::Ingest::extract(mime::parse(sample("ical/thunderbird-request.eml")));
        QVERIFY(t.calendar.contains("BEGIN:VCALENDAR"));
        QVERIFY(!t.text.contains(QStringLiteral("VCALENDAR")));
        QVERIFY(t.text.contains(QStringLiteral("has invited you")));
    }

    void invitationRoundTrip()
    {
        const QString uid = aliceInvites();
        QVERIFY(!uid.isEmpty());
        // Alice's calendar has it, organized by her, nobody answered yet.
        auto onAlice = Calendar::get(m_alice->ctx(), uid);
        QVERIFY(onAlice);
        QVERIFY(Calendar::organizedByUs(m_alice->ctx(), *onAlice));
        QCOMPARE(onAlice->attendees.size(), 2);
        QCOMPARE(onAlice->sequence, 0);
        // Alice's Sent copy carries the part and shows her attendees.
        const qint64 sent = find(m_alice, QStringLiteral("Invitation: Design review"), tag::Sent);
        QVERIFY(sent);
        const auto sentInv = Calendar::invitation(m_alice->ctx(), detail(m_alice, sent));
        QVERIFY(sentInv && sentInv->organizerIsSelf && sentInv->inCalendar);
        QCOMPARE(sentInv->attendees.size(), 2);

        // Bob: a card he can act on, nothing in his calendar yet.
        const qint64 bobId = find(m_bob, QStringLiteral("Invitation: Design review"));
        QVERIFY(bobId);
        const MessageDetail d = detail(m_bob, bobId);
        QVERIFY(!d.calendar.isEmpty());
        QCOMPARE(d.calMethod, QStringLiteral("REQUEST"));
        QCOMPARE(d.calUid, uid);
        QVERIFY(!d.bodyText.contains(QStringLiteral("VCALENDAR")));
        QVERIFY(d.bodyText.contains(QStringLiteral("alice invites you")));
        QVERIFY(d.attachments.isEmpty());
        QVERIFY(!d.structured.isEmpty()); // the JSON-LD Event beside it
        QCOMPARE(mail::Structured::parse(d.structured).size(), 1);
        QVERIFY(Calendar::actionable(m_bob->ctx(), d));
        auto inv = Calendar::invitation(m_bob->ctx(), d);
        QVERIFY(inv && inv->organizerIsSender && !inv->organizerIsSelf && !inv->inCalendar);
        QCOMPARE(inv->event.summary, QStringLiteral("Design review"));
        QVERIFY(Calendar::list(m_bob->ctx(), utc(2026, 11, 1, 0, 0), utc(2026, 12, 1, 0, 0)).isEmpty());

        // Bob accepts: in his calendar with his answer and the default
        // reminder; the answer reaches Alice.
        const Calendar::SendResult r = Calendar::rsvp(m_bob->ctx(), bobId, ICal::PartStat::Accepted);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(r.uid, uid);
        auto onBob = Calendar::get(m_bob->ctx(), uid);
        QVERIFY(onBob);
        QCOMPARE(Calendar::ourStatus(m_bob->ctx(), *onBob), std::optional<ICal::PartStat>(ICal::PartStat::Accepted));
        QCOMPARE(onBob->reminderMinutes, std::optional<int>(15));
        QCOMPARE(onBob->sourceMid, d.messageId);
        QCOMPARE(Calendar::list(m_bob->ctx(), utc(2026, 11, 1, 0, 0), utc(2026, 12, 1, 0, 0)).size(), 1);
        inv = Calendar::invitation(m_bob->ctx(), detail(m_bob, bobId));
        QVERIFY(inv->inCalendar);
        QCOMPARE(inv->ourStatus, std::optional<ICal::PartStat>(ICal::PartStat::Accepted));
        sync(m_bob);
        sync(m_alice);
        onAlice = Calendar::get(m_alice->ctx(), uid);
        for (const ICal::Attendee &a : onAlice->attendees)
            QCOMPARE(a.partstat, a.addr == bob() ? ICal::PartStat::Accepted : ICal::PartStat::NeedsAction);
        QVERIFY(onAlice->replied.contains(bob()));
        const qint64 answer = find(m_alice, QStringLiteral("Accepted: Design review"));
        QVERIFY(answer);
        const auto ans = Calendar::invitation(m_alice->ctx(), detail(m_alice, answer));
        QVERIFY(ans);
        QCOMPARE(ans->method, ICal::Method::Reply);
        QCOMPARE(ans->replyFrom, QStringLiteral("bob"));
        QCOMPARE(ans->replyStatus, ICal::PartStat::Accepted);
        QVERIFY(!Calendar::actionable(m_alice->ctx(), detail(m_alice, answer)));
        // The Sent copy now shows Bob's answer.
        QCOMPARE(Calendar::invitation(m_alice->ctx(), detail(m_alice, sent))->attendees[0].partstat,
                 ICal::PartStat::Accepted);
    }

    void updatesAndCancellations()
    {
        const QString uid = aliceInvites();
        Calendar::rsvp(m_bob->ctx(), find(m_bob, QStringLiteral("Invitation: Design review")), ICal::PartStat::Accepted);
        sync(m_bob);
        sync(m_alice);

        // Alice moves the meeting: sequence 1, a new request to both; Bob's
        // copy follows and asks him again; Carol, who never acted, still has
        // nothing in her calendar.
        auto e = *Calendar::get(m_alice->ctx(), uid);
        e.start = utc(2026, 11, 11, 14, 0);
        e.end = utc(2026, 11, 11, 15, 0);
        auto r = Calendar::save(m_alice->ctx(), e, true);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(Calendar::get(m_alice->ctx(), uid)->sequence, 1);
        QCOMPARE(Calendar::get(m_alice->ctx(), uid)->attendees[0].partstat, ICal::PartStat::Accepted); // kept
        sync(m_alice);
        sync(m_bob);
        sync(m_carol);
        auto onBob = *Calendar::get(m_bob->ctx(), uid);
        QCOMPARE(onBob.start, utc(2026, 11, 11, 14, 0));
        QCOMPARE(onBob.sequence, 1);
        QCOMPARE(Calendar::ourStatus(m_bob->ctx(), onBob), std::optional<ICal::PartStat>(ICal::PartStat::NeedsAction));
        QCOMPARE(onBob.reminderMinutes, std::optional<int>(15));
        QVERIFY(find(m_bob, QStringLiteral("Updated invitation: Design review")));
        QVERIFY(!Calendar::get(m_carol->ctx(), uid));
        // The first invitation is outdated on Bob's side now.
        const auto old = Calendar::invitation(m_bob->ctx(), detail(m_bob, find(m_bob, QStringLiteral("Invitation: Design review"))));
        QVERIFY(old->outdated);
        QVERIFY(!Calendar::rsvp(m_bob->ctx(), find(m_bob, QStringLiteral("Invitation: Design review")), ICal::PartStat::Accepted).error.isEmpty());
        // Bob answers the new one.
        r = Calendar::rsvp(m_bob->ctx(), find(m_bob, QStringLiteral("Updated invitation: Design review")), ICal::PartStat::Tentative);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        sync(m_bob);
        sync(m_alice);
        QCOMPARE(Calendar::get(m_alice->ctx(), uid)->attendees[0].partstat, ICal::PartStat::Tentative);
        QVERIFY(find(m_alice, QStringLiteral("Tentative: Design review")));

        // Alice takes Carol off: Carol hears it is cancelled, Bob gets the
        // list without her.
        e = *Calendar::get(m_alice->ctx(), uid);
        e.attendees = {e.attendees[0]};
        r = Calendar::save(m_alice->ctx(), e, true);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(r.queued.size(), 2);
        sync(m_alice);
        sync(m_bob);
        sync(m_carol);
        QVERIFY(find(m_carol, QStringLiteral("Cancelled: Design review")));
        QCOMPARE(Calendar::get(m_bob->ctx(), uid)->attendees.size(), 1);
        QCOMPARE(Calendar::get(m_bob->ctx(), uid)->sequence, 2);
        // Carol adds the cancelled copy by hand: it is cancelled for her.
        r = Calendar::addFromMessage(m_carol->ctx(), find(m_carol, QStringLiteral("Cancelled: Design review")));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(Calendar::get(m_carol->ctx(), uid)->status, ICal::Status::Cancelled);

        // Alice cancels: gone from Bob's lists, the row says why.
        r = Calendar::cancel(m_alice->ctx(), uid, true);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        sync(m_alice);
        sync(m_bob);
        QCOMPARE(Calendar::get(m_bob->ctx(), uid)->status, ICal::Status::Cancelled);
        QVERIFY(Calendar::list(m_bob->ctx(), utc(2026, 11, 1, 0, 0), utc(2026, 12, 1, 0, 0)).isEmpty());
        const auto cancelInv = Calendar::invitation(m_bob->ctx(), detail(m_bob, find(m_bob, QStringLiteral("Cancelled: Design review"))));
        QVERIFY(cancelInv && cancelInv->method == ICal::Method::Cancel && cancelInv->cancelled);
        // An older request arriving late does not bring it back.
        Draft stale;
        stale.to = {{QString(), bob()}};
        stale.subject = QStringLiteral("Invitation: Design review");
        stale.text = QStringLiteral("old");
        ICal::Event ev = *Calendar::get(m_alice->ctx(), uid);
        ev.sequence = 0;
        ev.status = ICal::Status::Confirmed;
        stale.calendar = ICal::write({ICal::Method::Request, {ev}});
        stale.calendarMethod = "REQUEST";
        mail::Compose::queue(m_alice->ctx(), stale);
        sync(m_alice);
        sync(m_bob);
        QCOMPARE(Calendar::get(m_bob->ctx(), uid)->status, ICal::Status::Cancelled);
    }

    void whoMayAct()
    {
        // Without gating a stranger's invitation reaches the Inbox, but it is
        // inert until the person trusts them; adding it by hand keeps only
        // the facts, with no organizer to answer to.
        mail::Preferences::set(m_bob->ctx(), cfg::Gating, QStringLiteral("0"));
        Calendar::Event e;
        e.summary = QStringLiteral("Free prize");
        e.start = utc(2026, 11, 20, 9, 0);
        e.end = utc(2026, 11, 20, 10, 0);
        e.attendees = {{bob(), QString(), ICal::PartStat::NeedsAction, true}};
        Calendar::save(m_dana->ctx(), e, true);
        sync(m_dana);
        sync(m_bob);
        qint64 id = find(m_bob, QStringLiteral("Invitation: Free prize"));
        QVERIFY(id);
        QVERIFY(!detail(m_bob, id).held);
        QVERIFY(!Calendar::actionable(m_bob->ctx(), detail(m_bob, id)));
        QVERIFY(!Calendar::rsvp(m_bob->ctx(), id, ICal::PartStat::Accepted).error.isEmpty());
        auto r = Calendar::addFromMessage(m_bob->ctx(), id);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QVERIFY(Calendar::get(m_bob->ctx(), r.uid)->organizer.addr.isEmpty());
        Calendar::removeLocal(m_bob->ctx(), r.uid);
        mail::Organize::accept(m_bob->ctx(), dana());
        QVERIFY(Calendar::actionable(m_bob->ctx(), detail(m_bob, id)));
        // With gating, the same invitation waits in Unverified and nothing
        // can be done with it there.
        mail::Preferences::set(m_carol->ctx(), cfg::Gating, QStringLiteral("1"));
        e.attendees = {{carol(), QString(), ICal::PartStat::NeedsAction, true}};
        e.uid.clear();
        Calendar::save(m_dana->ctx(), e, true);
        sync(m_dana);
        sync(m_carol);
        id = find(m_carol, QStringLiteral("Invitation: Free prize"), tag::Unverified);
        QVERIFY(id);
        QVERIFY(!Calendar::actionable(m_carol->ctx(), detail(m_carol, id)));
        QVERIFY(!Calendar::addFromMessage(m_carol->ctx(), id).error.isEmpty());

        // The sender must be the organizer the calendar names: Carol forwarding
        // Alice's invitation gets no buttons, but the facts can still be added
        // as a note of Bob's own, without an organizer to answer to.
        Draft forged;
        forged.to = {{QString(), bob()}};
        forged.subject = QStringLiteral("Invitation: Not mine");
        forged.text = QStringLiteral("x");
        ICal::Event ev;
        ev.uid = QStringLiteral("forged@x.test");
        ev.summary = QStringLiteral("Not mine");
        ev.start = utc(2026, 11, 21, 9, 0);
        ev.end = utc(2026, 11, 21, 10, 0);
        ev.organizer = {alice(), QStringLiteral("Alice"), ICal::PartStat::Accepted, false};
        ev.attendees = {{bob(), QString(), ICal::PartStat::NeedsAction, true}};
        forged.calendar = ICal::write({ICal::Method::Request, {ev}});
        forged.calendarMethod = "REQUEST";
        mail::Compose::queue(m_carol->ctx(), forged);
        sync(m_carol);
        sync(m_bob);
        id = find(m_bob, QStringLiteral("Invitation: Not mine"));
        QVERIFY(id);
        QVERIFY(!Calendar::actionable(m_bob->ctx(), detail(m_bob, id)));
        QVERIFY(!Calendar::invitation(m_bob->ctx(), detail(m_bob, id))->organizerIsSender);
        QVERIFY(Calendar::addFromMessage(m_bob->ctx(), id).error.isEmpty());
        auto added = Calendar::get(m_bob->ctx(), QStringLiteral("forged@x.test"));
        QVERIFY(added);
        QVERIFY(added->organizer.addr.isEmpty());
        QVERIFY(added->attendees.isEmpty());
        // Nor may anyone but that organizer change it afterwards: Carol's
        // "update" of Alice's event leaves Bob's copy alone.
        const QString uid = aliceInvites();
        Calendar::rsvp(m_bob->ctx(), find(m_bob, QStringLiteral("Invitation: Design review")), ICal::PartStat::Accepted);
        Draft meddle;
        meddle.to = {{QString(), bob()}};
        meddle.subject = QStringLiteral("Updated invitation: Design review");
        meddle.text = QStringLiteral("x");
        ICal::Event m = *Calendar::get(m_bob->ctx(), uid);
        m.sequence = 7;
        m.summary = QStringLiteral("Hijacked");
        m.organizer = {carol(), QString(), ICal::PartStat::Accepted, false};
        meddle.calendar = ICal::write({ICal::Method::Request, {m}});
        meddle.calendarMethod = "REQUEST";
        mail::Compose::queue(m_carol->ctx(), meddle);
        sync(m_carol);
        sync(m_bob);
        QCOMPARE(Calendar::get(m_bob->ctx(), uid)->summary, QStringLiteral("Design review"));
        QCOMPARE(Calendar::get(m_bob->ctx(), uid)->sequence, 0);
    }

    void answersAreTheirOwn()
    {
        const QString uid = aliceInvites();
        // Dana answers in Bob's name: ignored, she was not invited and the
        // answer is not hers.
        Draft fake;
        fake.to = {{QString(), alice()}};
        fake.subject = QStringLiteral("Accepted: Design review");
        fake.text = QStringLiteral("x");
        ICal::Event r;
        r.uid = uid;
        r.start = utc(2026, 11, 10, 9, 0);
        r.stamp = QDateTime::currentDateTimeUtc();
        r.attendees = {{bob(), QString(), ICal::PartStat::Accepted, false}};
        fake.calendar = ICal::write({ICal::Method::Reply, {r}});
        fake.calendarMethod = "REPLY";
        mail::Compose::queue(m_dana->ctx(), fake);
        sync(m_dana);
        sync(m_alice);
        QCOMPARE(Calendar::get(m_alice->ctx(), uid)->attendees[0].partstat, ICal::PartStat::NeedsAction);
        // Bob accepts; an older answer of his arriving later does not undo it.
        Calendar::rsvp(m_bob->ctx(), find(m_bob, QStringLiteral("Invitation: Design review")), ICal::PartStat::Accepted);
        sync(m_bob);
        sync(m_alice);
        QCOMPARE(Calendar::get(m_alice->ctx(), uid)->attendees[0].partstat, ICal::PartStat::Accepted);
        Draft older;
        older.to = {{QString(), alice()}};
        older.subject = QStringLiteral("Declined: Design review");
        older.text = QStringLiteral("x");
        r.stamp = utc(2026, 1, 1, 0, 0);
        r.attendees = {{bob(), QString(), ICal::PartStat::Declined, false}};
        older.calendar = ICal::write({ICal::Method::Reply, {r}});
        older.calendarMethod = "REPLY";
        mail::Compose::queue(m_bob->ctx(), older);
        sync(m_bob);
        sync(m_alice);
        QCOMPARE(Calendar::get(m_alice->ctx(), uid)->attendees[0].partstat, ICal::PartStat::Accepted);
        // Taking the event out of the calendar makes later answers moot.
        Calendar::removeLocal(m_alice->ctx(), uid);
        QVERIFY(!Calendar::get(m_alice->ctx(), uid));
    }

    void encryptedInvitation()
    {
        // Alice and Bob have met, so what they write is encrypted.
        Draft hello;
        hello.to = {{QString(), bob()}};
        hello.subject = QStringLiteral("hello");
        hello.text = QStringLiteral("hello");
        mail::Compose::queue(m_alice->ctx(), hello);
        sync(m_alice);
        sync(m_bob);
        Draft back = mail::Compose::reply(m_bob->ctx(), find(m_bob, QStringLiteral("hello")), false);
        back.text = QStringLiteral("hi");
        mail::Compose::queue(m_bob->ctx(), back);
        sync(m_bob);
        sync(m_alice);

        Calendar::Event e;
        e.summary = QStringLiteral("Secret plans");
        e.start = utc(2026, 11, 12, 9, 0);
        e.end = utc(2026, 11, 12, 10, 0);
        e.attendees = {{bob(), QStringLiteral("Bob"), ICal::PartStat::NeedsAction, true}};
        const auto r = Calendar::save(m_alice->ctx(), e, true);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        sync(m_alice);
        sync(m_bob);
        const qint64 id = find(m_bob, QStringLiteral("Invitation: Secret plans"));
        QVERIFY(id);
        const MessageDetail d = detail(m_bob, id);
        QVERIFY(d.encrypted);
        QVERIFY(!d.calendar.isEmpty());
        QVERIFY(Calendar::actionable(m_bob->ctx(), d));
        // The server never saw the summary.
        for (const auto &env : m_srv->envelopes())
            QVERIFY(!env.data.contains("Secret plans"));
        const auto a = Calendar::rsvp(m_bob->ctx(), id, ICal::PartStat::Accepted);
        QVERIFY2(a.error.isEmpty(), qPrintable(a.error));
        sync(m_bob);
        sync(m_alice);
        const qint64 ans = find(m_alice, QStringLiteral("Accepted: Secret plans"));
        QVERIFY(ans);
        QVERIFY(detail(m_alice, ans).encrypted);
        QCOMPARE(Calendar::get(m_alice->ctx(), r.uid)->attendees[0].partstat, ICal::PartStat::Accepted);
    }

    void fromStructuredCard()
    {
        const auto items = mail::Structured::parse(mail::Structured::accept(sample("structured-flight.json")));
        QCOMPARE(items.size(), 1);
        ICal::Event e;
        e.summary = items[0].title;
        e.start = items[0].start;
        e.end = items[0].end;
        e.allDay = items[0].allDay;
        e.location = items[0].place;
        const QString uid = Calendar::addEvent(m_alice->ctx(), e, QStringLiteral("<flight@air.example>"));
        const auto saved = Calendar::get(m_alice->ctx(), uid);
        QVERIFY(saved);
        QCOMPARE(saved->summary, items[0].title);
        QCOMPARE(saved->start, items[0].start);
        QVERIFY(saved->attendees.isEmpty());
        QCOMPARE(saved->sourceMid, QStringLiteral("<flight@air.example>"));
        QCOMPARE(saved->reminderMinutes, std::optional<int>(15));
        QVERIFY(Calendar::exportIcs(m_alice->ctx(), uid).contains("METHOD:PUBLISH"));
    }

    void reminders()
    {
        auto &ctx = m_alice->ctx();
        const QDateTime now = QDateTime::currentDateTimeUtc();
        Calendar::Event soon;
        soon.summary = QStringLiteral("Soon");
        soon.start = now.addSecs(600);
        soon.end = now.addSecs(1200);
        soon.reminderMinutes = 15;
        Calendar::saveLocal(ctx, soon);
        Calendar::Event later;
        later.summary = QStringLiteral("Later");
        later.start = now.addSecs(7200);
        later.end = now.addSecs(7800);
        later.reminderMinutes = 30;
        Calendar::saveLocal(ctx, later);
        Calendar::Event silent;
        silent.summary = QStringLiteral("Silent");
        silent.start = now.addSecs(300);
        silent.end = now.addSecs(600);
        Calendar::saveLocal(ctx, silent);
        const auto due = Calendar::dueReminders(ctx, now.toMSecsSinceEpoch());
        QCOMPARE(due.size(), 1);
        QCOMPARE(due[0].event.summary, QStringLiteral("Soon"));
        QVERIFY(Calendar::dueReminders(ctx, now.toMSecsSinceEpoch()).isEmpty()); // shown once
        const auto next = Calendar::nextReminderMs(ctx, now.toMSecsSinceEpoch());
        QVERIFY(next);
        QCOMPARE(*next, later.start.addSecs(-1800).toMSecsSinceEpoch());
    }

    // Dragging an event in the week (ADR 0025, amendment 2026-11-10).
    void reschedule()
    {
        auto &ctx = m_alice->ctx();
        // A note of Alice's own moves, and keeps the rest of itself.
        Calendar::Event note;
        note.summary = QStringLiteral("Dentist");
        note.location = QStringLiteral("Main St 4");
        note.start = utc(2026, 11, 10, 8, 0);
        note.end = utc(2026, 11, 10, 8, 45);
        note.reminderMinutes = 30;
        const QString own = Calendar::saveLocal(ctx, note);
        QVERIFY(Calendar::movable(ctx, *Calendar::get(ctx, own)));
        auto r = Calendar::reschedule(ctx, own, utc(2026, 11, 12, 15, 0), utc(2026, 11, 12, 15, 45), true);
        QCOMPARE(r.uid, own);
        QVERIFY(r.queued.isEmpty()); // nobody to tell
        auto moved = *Calendar::get(ctx, own);
        QCOMPARE(moved.start, utc(2026, 11, 12, 15, 0));
        QCOMPARE(moved.end, utc(2026, 11, 12, 15, 45));
        QCOMPARE(moved.summary, QStringLiteral("Dentist"));
        QCOMPARE(moved.location, QStringLiteral("Main St 4"));
        QCOMPARE(moved.reminderMinutes, std::optional<int>(30));
        // An end that is not after the start, or no time at all: refused.
        QVERIFY(Calendar::reschedule(ctx, own, utc(2026, 11, 12, 15, 0), utc(2026, 11, 12, 15, 0), true).uid.isEmpty());
        QVERIFY(Calendar::reschedule(ctx, own, QDateTime(), utc(2026, 11, 12, 15, 0), true).uid.isEmpty());
        QVERIFY(Calendar::reschedule(ctx, QStringLiteral("no-such-event"), utc(2026, 11, 12, 15, 0), utc(2026, 11, 12, 16, 0), true).uid.isEmpty());
        QCOMPARE(Calendar::get(ctx, own)->start, utc(2026, 11, 12, 15, 0));

        // A meeting Alice organizes: "save only" moves it here and mails
        // nobody; sending asks the attendees again with a new sequence.
        const QString uid = aliceInvites();
        Calendar::rsvp(m_bob->ctx(), find(m_bob, QStringLiteral("Invitation: Design review")), ICal::PartStat::Accepted);
        sync(m_bob);
        sync(m_alice);
        r = Calendar::reschedule(ctx, uid, utc(2026, 11, 10, 13, 0), utc(2026, 11, 10, 14, 0), false);
        QCOMPARE(r.uid, uid);
        QVERIFY(r.queued.isEmpty());
        QCOMPARE(Calendar::get(ctx, uid)->start, utc(2026, 11, 10, 13, 0));
        QCOMPARE(Calendar::get(ctx, uid)->sequence, 0);
        r = Calendar::reschedule(ctx, uid, utc(2026, 11, 11, 14, 0), utc(2026, 11, 11, 15, 30), true);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(r.queued.size(), 1);
        QCOMPARE(Calendar::get(ctx, uid)->sequence, 1);
        QCOMPARE(Calendar::get(ctx, uid)->attendees.size(), 2);
        sync(m_alice);
        sync(m_bob);
        auto onBob = *Calendar::get(m_bob->ctx(), uid);
        QCOMPARE(onBob.start, utc(2026, 11, 11, 14, 0));
        QCOMPARE(onBob.end, utc(2026, 11, 11, 15, 30));
        QCOMPARE(Calendar::ourStatus(m_bob->ctx(), onBob), std::optional<ICal::PartStat>(ICal::PartStat::NeedsAction));
        // Dropped where it already is: nothing is sent and the sequence stays.
        r = Calendar::reschedule(ctx, uid, utc(2026, 11, 11, 14, 0), utc(2026, 11, 11, 15, 30), true);
        QCOMPARE(r.uid, uid);
        QVERIFY(r.queued.isEmpty());
        QCOMPARE(Calendar::get(ctx, uid)->sequence, 1);

        // Bob's copy of Alice's meeting is hers to move, not his.
        QVERIFY(!Calendar::movable(m_bob->ctx(), onBob));
        QVERIFY(Calendar::reschedule(m_bob->ctx(), uid, utc(2026, 11, 12, 9, 0), utc(2026, 11, 12, 10, 0), true).uid.isEmpty());
        QCOMPARE(Calendar::get(m_bob->ctx(), uid)->start, utc(2026, 11, 11, 14, 0));

        // One occurrence of a series cannot be moved alone, so none is.
        Calendar::Event weekly;
        weekly.summary = QStringLiteral("Standup");
        weekly.start = utc(2026, 11, 9, 9, 30);
        weekly.end = utc(2026, 11, 9, 9, 45);
        weekly.rrule = QStringLiteral("FREQ=WEEKLY;BYDAY=MO");
        const QString series = Calendar::saveLocal(ctx, weekly);
        QVERIFY(!Calendar::movable(ctx, *Calendar::get(ctx, series)));
        QVERIFY(Calendar::reschedule(ctx, series, utc(2026, 11, 16, 11, 0), utc(2026, 11, 16, 11, 15), true).uid.isEmpty());
        QCOMPARE(Calendar::get(ctx, series)->start, utc(2026, 11, 9, 9, 30));

        // Nor an all-day event, nor a cancelled one.
        Calendar::Event offsite;
        offsite.summary = QStringLiteral("Offsite");
        offsite.allDay = true;
        offsite.start = utc(2026, 11, 20, 0, 0);
        offsite.end = utc(2026, 11, 21, 0, 0);
        const QString day = Calendar::saveLocal(ctx, offsite);
        QVERIFY(Calendar::reschedule(ctx, day, utc(2026, 11, 22, 9, 0), utc(2026, 11, 22, 10, 0), true).uid.isEmpty());
        Calendar::cancel(ctx, own, false);
        QVERIFY(Calendar::reschedule(ctx, own, utc(2026, 11, 13, 9, 0), utc(2026, 11, 13, 10, 0), true).uid.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestCalendar)
#include "tst_calendar.moc"
