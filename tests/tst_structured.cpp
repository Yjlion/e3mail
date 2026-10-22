// SPDX-License-Identifier: MPL-2.0
#include "FakeMailServer.h"
#include "crypto/Pgp.h"
#include "engine/AccountManager.h"
#include "engine/MailWorker.h"
#include "mail/Compose.h"
#include "mail/Contacts.h"
#include "mail/Ingest.h"
#include "mail/Keyring.h"
#include "mail/Search.h"
#include "mail/Structured.h"
#include "mime/Builder.h"
#include "mime/Part.h"
#include "store/Config.h"
#include "store/Database.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace e3;
using mail::Structured;

namespace {

// Bookings as senders write them, kept as files.
QByteArray sample(const char *name)
{
    QFile f(QStringLiteral(E3_TEST_DATA "/structured-%1.json").arg(QLatin1String(name)));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class TestStructured : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<FakeMailServer> m_srv;
    std::unique_ptr<AccountManager> m_mgr;

    Account *makeAccount(const QString &addr)
    {
        m_srv->addUser(addr, QStringLiteral("pw"));
        Account *a = m_mgr->create();
        Account::Settings s;
        s.addr = addr;
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

    // A message with a text part and the JSON-LD as its machine-readable
    // alternative (SML, full representation), signed by `from`'s key if asked.
    QByteArray structuredMail(Account *from, const QString &to, const QByteArray &json, bool sign,
                              const QString &id)
    {
        const QString addr = from->ctx().config.get(cfg::Addr);
        const QByteArray content = "Content-Type: multipart/alternative; boundary=\"alt\"\r\n\r\n"
                                   "--alt\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nYour flight is booked.\r\n"
                                   "--alt\r\nContent-Type: application/ld+json\r\nContent-Purpose: Machine-readable\r\n\r\n"
            + json + "\r\n--alt--\r\n";
        QByteArray head = "From: " + addr.toUtf8() + "\r\nTo: " + to.toUtf8() + "\r\nSubject: Booking\r\nMessage-ID: <"
            + id.toUtf8() + ">\r\nDate: Wed, 21 Oct 2026 10:00:00 +0000\r\nMIME-Version: 1.0\r\n";
        if (!sign)
            return head + content;
        const QString fpr = mail::Keyring::ensureSelfKey(from->ctx(), addr);
        QByteArray canon = content;
        canon.replace("\r\n", "\n").replace("\n", "\r\n");
        const QByteArray sig = from->ctx().pgp.signDetached(canon, fpr);
        return head
            + "Content-Type: multipart/signed; protocol=\"application/pgp-signature\"; micalg=pgp-sha256; "
              "boundary=\"sig\"\r\n\r\n--sig\r\n"
            + content + "\r\n--sig\r\nContent-Type: application/pgp-signature\r\n\r\n" + sig + "\r\n--sig--\r\n";
    }

    qint64 idOf(Account *a, const QString &mid)
    {
        return a->ctx().db.queryInt("SELECT id FROM messages WHERE message_id=?", mid).value_or(0);
    }

private Q_SLOTS:
    void init()
    {
        qputenv("E3MAIL_NO_KEYCHAIN", "1");
        m_dir = std::make_unique<QTemporaryDir>();
        m_srv = std::make_unique<FakeMailServer>();
        m_mgr = std::make_unique<AccountManager>(m_dir->path());
    }

    void cleanup()
    {
        m_mgr.reset();
        m_srv.reset();
        m_dir.reset();
    }

    void flightReads()
    {
        const auto items = Structured::parse(Structured::accept(sample("flight")));
        QCOMPARE(items.size(), 1);
        const Structured::Item &f = items[0];
        QCOMPARE(f.kind, Structured::Kind::Flight);
        QCOMPARE(f.title, QStringLiteral("Flight EX110 · LHR → LIS"));
        QCOMPARE(f.start, QDateTime(QDate(2026, 11, 3), QTime(8, 40), QTimeZone::UTC));
        QCOMPARE(f.end, QDateTime(QDate(2026, 11, 3), QTime(11, 15), QTimeZone::UTC));
        QCOMPARE(f.link, QStringLiteral("https://air.example/trips/RXJ34P"));
        QStringList values;
        for (const auto &field : f.fields)
            values.append(field.value);
        QVERIFY(values.contains(QStringLiteral("RXJ34P")));
        QVERIFY(values.contains(QStringLiteral("14C")));
        QVERIFY(values.contains(QStringLiteral("Ada Lovelace")));

        const QByteArray ics = Structured::toIcs(f, QStringLiteral("x@e3mail"));
        QVERIFY(ics.startsWith("BEGIN:VCALENDAR\r\n"));
        QVERIFY(ics.contains("DTSTART:20261103T084000Z\r\n"));
        QVERIFY(ics.contains("DTEND:20261103T111500Z\r\n"));
        QVERIFY(ics.contains("SUMMARY:Flight EX110 · LHR → LIS"));
        for (const QByteArray &line : ics.split('\n'))
            QVERIFY(line.size() <= 76);
    }

    void onlyWebLinksAndPlainText()
    {
        const auto parcel = Structured::parse(Structured::accept(sample("parcel")));
        QCOMPARE(parcel.size(), 1);
        QCOMPARE(parcel[0].kind, Structured::Kind::Parcel);
        QVERIFY(parcel[0].link.isEmpty()); // javascript: is not a link
        QVERIFY(parcel[0].allDay); // a date, not midnight
        QCOMPARE(parcel[0].start.date(), QDate(2026, 11, 5));
        // Markup stays text; bidi overrides and control characters go.
        const auto ev = Structured::parse(Structured::accept(
            "{\"@type\":\"Event\",\"name\":\"<b>Party</b>\\u202e\\u0007 tonight\",\"startDate\":\"2026-12-31\"}"));
        QCOMPARE(ev.size(), 1);
        QCOMPARE(ev[0].title, QStringLiteral("<b>Party</b> tonight"));
        QVERIFY(ev[0].allDay);
        QVERIFY(Structured::toIcs(ev[0], QStringLiteral("u")).contains("DTSTART;VALUE=DATE:20261231"));
    }

    void hostileInputIsRefused()
    {
        QVERIFY(Structured::accept("not json").isEmpty());
        QVERIFY(Structured::accept(QByteArray(Structured::kMaxBytes + 1, ' ') + "{}").isEmpty());
        QByteArray deep;
        for (int i = 0; i < 50; ++i)
            deep += "{\"a\":";
        deep += "1";
        for (int i = 0; i < 50; ++i)
            deep += "}";
        QVERIFY(Structured::accept(deep).isEmpty());
        // Types outside the list are left out; a graph is read node by node,
        // up to a limit.
        QByteArray graph = R"({"@graph":[{"@type":"Recipe","name":"soup"})";
        for (int i = 0; i < 20; ++i)
            graph += R"(,{"@type":"Event","name":"e","startDate":"2026-01-01"})";
        graph += "]}";
        const auto items = Structured::parse(Structured::accept(graph));
        QCOMPARE(items.size(), Structured::kMaxItems);
        QCOMPARE(items[0].kind, Structured::Kind::Event);
        // A long value is cut.
        const auto longName = Structured::parse(
            Structured::accept("{\"@type\":\"Event\",\"name\":\"" + QByteArray(5000, 'x') + "\"}"));
        QCOMPARE(longName[0].title.size(), Structured::kMaxText);
    }

    void legacyHtml()
    {
        const QString html = QStringLiteral("<html><head><script type=\"application/ld+json\">") + QString::fromUtf8(sample("parcel"))
            + QStringLiteral("</script></head><body><p>Shipped.</p></body></html>");
        QVERIFY(!Structured::parse(Structured::accept(Structured::fromHtml(html))).isEmpty());
        QVERIFY(Structured::fromHtml(QStringLiteral("<p>nothing</p>")).isEmpty());
    }

    // Partial representation: the JSON-LD beside the text in a
    // multipart/related is read, not listed; a .json the sender attaches as a
    // file stays an attachment.
    void partNotListed()
    {
        const mime::Part root = mime::parse(
            "Content-Type: multipart/mixed; boundary=\"m\"\r\n\r\n"
            "--m\r\nContent-Type: multipart/related; boundary=\"r\"\r\n\r\n"
            "--r\r\nContent-Type: text/plain\r\n\r\nYour parcel is on its way.\r\n"
            "--r\r\nContent-Type: application/ld+json\r\nContent-Purpose: Machine-readable\r\n\r\n"
            + sample("parcel") + "\r\n--r--\r\n"
            "--m\r\nContent-Type: application/ld+json\r\nContent-Disposition: attachment; filename=\"data.json\"\r\n\r\n"
            "{\"a\":1}\r\n--m--\r\n");
        const auto c = mail::Ingest::extract(root);
        QVERIFY(c.text.contains(QLatin1String("on its way")));
        QVERIFY(c.structured.contains("ParcelDelivery"));
        QCOMPARE(c.attachments.size(), 1);
        QCOMPARE(c.attachments[0].filename, QStringLiteral("data.json"));
    }

    // A trusted, signed sender's card acts; anyone else's is shown inert.
    void trustedOrInert()
    {
        Account *ada = makeAccount(QStringLiteral("ada@x.test"));
        Account *air = makeAccount(QStringLiteral("air@x.test"));
        // Ada has the airline's key (it wrote first) and has it in her book.
        Draft d;
        d.to = {{QString(), QStringLiteral("ada@x.test")}};
        d.subject = QStringLiteral("hello");
        d.text = QStringLiteral("hello");
        mail::Compose::queue(air->ctx(), d);
        sync(air);
        sync(ada);
        mail::Contacts::create(ada->ctx(), QStringLiteral("air@x.test"), QString());

        m_srv->deliver(QStringLiteral("ada@x.test"),
                       structuredMail(air, QStringLiteral("ada@x.test"), sample("flight"), true, QStringLiteral("signed@x.test")));
        m_srv->deliver(QStringLiteral("ada@x.test"),
                       structuredMail(air, QStringLiteral("ada@x.test"), sample("flight"), false, QStringLiteral("plain@x.test")));
        sync(ada);

        const auto signedOne = *mail::Search::detail(ada->ctx(), idOf(ada, QStringLiteral("signed@x.test")));
        QVERIFY(signedOne.signedBySender);
        QVERIFY(!signedOne.structured.isEmpty());
        QVERIFY(signedOne.attachments.isEmpty()); // read, not listed
        QCOMPARE(signedOne.bodyText.trimmed(), QStringLiteral("Your flight is booked."));
        QVERIFY(Structured::actionable(ada->ctx(), signedOne));

        const auto unsignedOne = *mail::Search::detail(ada->ctx(), idOf(ada, QStringLiteral("plain@x.test")));
        QVERIFY(!unsignedOne.structured.isEmpty());
        QVERIFY(!Structured::actionable(ada->ctx(), unsignedOne));

        // The same signed mail from a sender Ada does not trust: inert.
        mail::Contacts::setTrust(ada->ctx(), QStringLiteral("air@x.test"), false);
        QVERIFY(!Structured::actionable(ada->ctx(), *mail::Search::detail(ada->ctx(), signedOne.id)));
    }
};

QTEST_GUILESS_MAIN(TestStructured)
#include "tst_structured.moc"
