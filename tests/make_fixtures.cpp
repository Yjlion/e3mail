// SPDX-License-Identifier: MPL-2.0
//
// e3mail-fixtures <data-dir>: builds a demonstration mailbox by having real
// accounts exchange real mail through the in-process fake mail system, then
// leaves the data directory behind for the app to open with --offline. Used
// for screenshots, which must never come from a real mailbox.

#include "FakeMailServer.h"
#include "engine/AccountManager.h"
#include "mail/Compose.h"
#include "mail/Signatures.h"
#include "mail/Contacts.h"
#include "mail/Organize.h"
#include "mail/Search.h"
#include "store/Config.h"
#include "store/Database.h"
#include "util/Paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>

using namespace e3;

namespace {

FakeMailServer *g_srv = nullptr;

Account *make(AccountManager &mgr, const QString &addr, const QString &name, const QString &protocol)
{
    g_srv->addUser(addr, QStringLiteral("pw"));
    Account *a = mgr.create();
    Account::Settings s;
    s.addr = addr;
    s.displayName = name;
    s.protocol = protocol;
    s.inHost = s.smtpHost = QStringLiteral("127.0.0.1");
    s.inPort = protocol == QLatin1String("pop3") ? g_srv->pop3Port() : g_srv->imapPort();
    s.inSecurity = s.smtpSecurity = QStringLiteral("plain");
    s.smtpPort = g_srv->smtpPort();
    s.inPassword = QStringLiteral("pw");
    a->configure(s);
    return a;
}

void fetchMail(Account *a)
{
    MailWorker::runOnce(a->dir(), a->credentials());
}

qint64 send(Account *from, const QString &to, const QString &subject, const QString &text, qint64 replyTo = 0,
            int importance = 0, const QList<OutgoingAttachmentRef> &att = {})
{
    Draft d = replyTo ? mail::Compose::reply(from->ctx(), replyTo, false) : Draft();
    if (!replyTo)
        d.to = {{QString(), to}};
    if (!subject.isEmpty())
        d.subject = subject;
    d.text = text + (replyTo ? d.text : QString());
    d.importance = importance;
    d.attachments = att;
    return mail::Compose::queue(from->ctx(), d);
}

qint64 latestFrom(Account *a, const QString &addr)
{
    SearchQuery q = SearchQuery::forTag(tag::All);
    for (const auto &m : mail::Search::list(a->ctx(), q)) {
        if (m.from.addr == addr)
            return m.id;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        qWarning("usage: e3mail-fixtures <data-dir>");
        return 2;
    }
    qputenv("E3MAIL_NO_KEYCHAIN", "1");
    const QString dir = QString::fromLocal8Bit(argv[1]);
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    Paths::setDataDir(dir);
    FakeMailServer srv;
    g_srv = &srv;

    AccountManager peers(dir + QStringLiteral("/peers"));
    AccountManager mgr(dir);
    Account *ada = make(mgr, QStringLiteral("ada@example.org"), QStringLiteral("Ada Lovelace"), QStringLiteral("imap"));
    mail::Signatures::save(ada->ctx(), {QString(), QStringLiteral("Work"),
                                        QStringLiteral("<p><b>Ada Lovelace</b><br><i>Analytical Engines</i>, London</p>"),
                                        QStringLiteral("Ada Lovelace\nAnalytical Engines, London")});
    mail::Signatures::save(ada->ctx(), {QString(), QStringLiteral("Personal"), QString(), QStringLiteral("Ada")});
    // Another device of Ada's, and no relay: screenshots never go online.
    ada->ctx().config.set(cfg::P2pRelay, QStringLiteral("off"));
    ada->ctx().config.set(cfg::P2pLoopback, QStringLiteral("1"));
    ada->ctx().config.set(cfg::DeviceName, QStringLiteral("Desktop"));
    ada->ctx().db.run("INSERT INTO devices(id, name, paired_at, node_id, last_seen) VALUES(?, ?, ?, ?, ?)",
                      QStringLiteral("laptop"), QStringLiteral("Laptop"), QDateTime::currentMSecsSinceEpoch(),
                      QString(64, u'a'), QDateTime::currentMSecsSinceEpoch() - 3 * 3600 * 1000);
    Account *mira = make(peers, QStringLiteral("mira@example.org"), QStringLiteral("Mira Dorn"), QStringLiteral("imap"));
    Account *tomas = make(peers, QStringLiteral("tomas@example.net"), QStringLiteral("Tomas Reyes"), QStringLiteral("pop3"));
    Account *stranger = make(peers, QStringLiteral("offers@deals.example"), QStringLiteral("Great Deals"), QStringLiteral("imap"));
    for (Account *a : {ada, mira, tomas, stranger})
        fetchMail(a);

    // Ada writes first; replies come back encrypted once keys are known.
    send(ada, QStringLiteral("mira@example.org"), QStringLiteral("Thursday's numbers"),
         QStringLiteral("Hi Mira,\n\nCould you send the reconciliation before Thursday?\n\nThanks"));
    send(ada, QStringLiteral("tomas@example.net"), QStringLiteral("Keys are rotated"),
         QStringLiteral("Tomas, the new keys are in place. Can you confirm on your side?"));
    fetchMail(ada);
    fetchMail(mira);
    fetchMail(tomas);
    mail::Organize::accept(mira->ctx(), QStringLiteral("ada@example.org"));
    mail::Organize::accept(tomas->ctx(), QStringLiteral("ada@example.org"));

    send(mira, QString(), QString(),
         QStringLiteral("Attaching the reconciliation. Everything balances except the second column, which is off by one."),
         latestFrom(mira, QStringLiteral("ada@example.org")), 1,
         {{QStringLiteral("reconciliation.csv"), QStringLiteral("text/csv"), "account,q3\nops,1200\nlab,830\n"}});
    fetchMail(mira);
    send(tomas, QString(), QString(), QStringLiteral("Done. New fingerprint is in the usual place; scan when we meet."),
         latestFrom(tomas, QStringLiteral("ada@example.org")));
    fetchMail(tomas);
    fetchMail(ada);

    // Ada answers Mira, and Mira follows up: a thread of four.
    send(ada, QString(), QString(), QStringLiteral("That matches what I have. One thing on the second column: it double-counts the lab."),
         latestFrom(ada, QStringLiteral("mira@example.org")));
    fetchMail(ada);
    fetchMail(mira);
    send(mira, QString(), QString(), QStringLiteral("Good catch — fixed and re-sent to finance."),
         latestFrom(mira, QStringLiteral("ada@example.org")));
    fetchMail(mira);

    // A cleartext newsletter-style message with HTML, and a stranger.
    Draft html;
    html.to = {{QString(), QStringLiteral("ada@example.org")}};
    html.subject = QStringLiteral("Notes from the standup");
    html.text = QStringLiteral("Short one today. Three things worth writing down:\n• the build is green\n• release on Friday\n• lunch is on Mira");
    html.html = QStringLiteral("<p>Short one today. Three things worth writing down:</p><ul><li>the build is <b>green</b></li>"
                               "<li>release on Friday</li><li>lunch is on Mira</li></ul>"
                               "<p><img src=\"https://tracker.example/pixel.gif\" alt=\"\"></p>");
    html.encryption = SendEncryption::Plaintext;
    mail::Compose::queue(mira->ctx(), html);
    fetchMail(mira);
    send(stranger, QStringLiteral("ada@example.org"), QStringLiteral("You have been selected!"),
         QStringLiteral("Claim your prize today. Limited time only."));
    fetchMail(stranger);
    fetchMail(ada);

    // Organise Ada's mailbox a little.
    MailContext &ctx = ada->ctx();
    const qint64 accounts = mail::Organize::createLabel(ctx, QStringLiteral("Accounts"), QStringLiteral("#2457d6"));
    const qint64 reading = mail::Organize::createLabel(ctx, QStringLiteral("Reading list"), QStringLiteral("#1a7f4b"));
    for (const auto &m : mail::Search::list(ctx, SearchQuery::forTag(tag::Inbox))) {
        if (m.subject.contains(QLatin1String("numbers")))
            mail::Organize::setLabel(ctx, m.id, accounts, true);
        if (m.subject.contains(QLatin1String("standup")))
            mail::Organize::setLabel(ctx, m.id, reading, true);
    }
    // Tomas was verified in person (SecureJoin arrives in a later phase; the
    // fixture records the outcome).
    if (const auto t = mail::Contacts::get(ctx, QStringLiteral("tomas@example.net")))
        mail::Contacts::setVerified(ctx, t->addr, t->fingerprint);
    ctx.db.run("UPDATE messages SET verified=1 WHERE from_addr='tomas@example.net' AND signed_fpr IS NOT NULL");
    // Ada asked Mira for a read receipt, and it came back; Tomas's answer
    // disappears a day after Ada read it, about twenty hours from now (P12;
    // the outcomes, recorded).
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    ctx.db.run("UPDATE messages SET mdn_to='ada@example.org' WHERE subject=? AND direction=1",
               QStringLiteral("Thursday's numbers"));
    ctx.db.run("INSERT INTO receipts(msg_id, addr, at) SELECT id, 'mira@example.org', ? FROM messages "
               "WHERE subject=? AND direction=1",
               now - 3600 * 1000, QStringLiteral("Thursday's numbers"));
    ctx.db.run("UPDATE messages SET ephemeral_seconds=86400, expires_at=?, unread=0 "
               "WHERE from_addr='tomas@example.net'",
               now + 20LL * 3600 * 1000);

    // Structured data (P13; the outcome, recorded): a flight on Tomas's
    // signed reply, which is trusted and so has buttons, and a parcel on the
    // stranger's offer, shown inert.
    ctx.db.run("UPDATE messages SET structured=? WHERE from_addr='tomas@example.net'",
               QStringLiteral(R"({"@type":"FlightReservation","reservationNumber":"RXJ34P",)"
                              R"("underName":{"name":"Ada Lovelace"},"reservedTicket":{"ticketedSeat":{"seatNumber":"14C"}},)"
                              R"("reservationFor":{"@type":"Flight","flightNumber":"110","airline":{"name":"Example Air","iataCode":"EX"},)"
                              R"("departureAirport":{"name":"London Heathrow","iataCode":"LHR"},"departureTime":"2026-11-03T08:40:00Z",)"
                              R"("arrivalAirport":{"name":"Lisbon","iataCode":"LIS"},"arrivalTime":"2026-11-03T11:15:00Z"},)"
                              R"("potentialAction":{"@type":"ViewAction","target":"https://air.example/trips/RXJ34P"}})"));
    ctx.db.run("UPDATE messages SET structured=? WHERE from_addr='offers@deals.example'",
               QStringLiteral(R"({"@type":"ParcelDelivery","carrier":{"name":"Swift Post"},"trackingNumber":"SP123",)"
                              R"("itemShipped":{"name":"Your prize"},"expectedArrivalUntil":"2026-11-05",)"
                              R"("trackingUrl":"https://deals.example/claim"})"));

    Draft draft;
    draft.to = {{QStringLiteral("Mira Dorn"), QStringLiteral("mira@example.org")}};
    draft.subject = QStringLiteral("Offsite agenda");
    draft.text = QStringLiteral("Rough agenda for the offsite:\n");
    mail::Compose::saveDraft(ctx, draft);

    QFile marker(dir + QStringLiteral("/first-run-acknowledged"));
    if (!marker.open(QIODevice::WriteOnly))
        return 1;
    QDir(dir + QStringLiteral("/peers")).removeRecursively();
    qInfo("fixture mailbox written to %s", qPrintable(dir));
    return 0;
}
