// SPDX-License-Identifier: MPL-2.0
#include "DeviceClone.h"
#include "FakeMailServer.h"
#include "engine/AccountManager.h"
#include "engine/MailWorker.h"
#include "mail/Compose.h"
#include "mail/Contacts.h"
#include "mail/Organize.h"
#include "mail/Preferences.h"
#include "mail/Search.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"
#include "sync/OpApply.h"
#include "util/Hlc.h"

#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace e3;

// Read receipts, disappearing mail and trust set by hand (ADR 0022), between
// real accounts over the in-process mail system.
class TestReceipts : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<FakeMailServer> m_srv;
    std::unique_ptr<AccountManager> m_mgr;
    std::unique_ptr<AccountManager> m_mgr2;
    Account *m_alice = nullptr;
    Account *m_bob = nullptr;

    static QString alice() { return QStringLiteral("alice@x.test"); }
    static QString bob() { return QStringLiteral("bob@x.test"); }

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

    // Alice writes to Bob; Bob fetches. Returns Bob's copy.
    qint64 aliceWrites(const QString &subject, bool receipt, int disappear = 0)
    {
        Draft d;
        d.to = {{QString(), bob()}};
        d.subject = subject;
        d.text = QStringLiteral("text of ") + subject;
        d.requestReceipt = receipt;
        d.ephemeralSeconds = disappear;
        mail::Compose::queue(m_alice->ctx(), d);
        sync(m_alice);
        sync(m_bob);
        for (const QString &t : {tag::Inbox, tag::Unverified}) {
            for (const auto &m : mail::Search::list(m_bob->ctx(), SearchQuery::forTag(t))) {
                if (m.subject == subject)
                    return m.id;
            }
        }
        return 0;
    }

    qint64 sentId(const QString &subject)
    {
        for (const auto &m : mail::Search::list(m_alice->ctx(), SearchQuery::forTag(tag::Sent))) {
            if (m.subject == subject)
                return m.id;
        }
        return 0;
    }

    int receiptsQueued(Account *a)
    {
        return int(a->ctx()
                       .db.queryInt("SELECT count(*) FROM messages WHERE hidden=1 AND subject='Receipt Notification'")
                       .value_or(-1));
    }

    // Bob reads, sends what is due; Alice fetches it.
    void bobReads(qint64 id)
    {
        mail::Organize::markRead(m_bob->ctx(), id, true);
        sync(m_bob);
        sync(m_alice);
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
    }

    void cleanup()
    {
        m_mgr2.reset();
        m_mgr.reset();
        m_srv.reset();
        m_dir.reset();
    }

    void receiptRoundTrip()
    {
        mail::Contacts::create(m_bob->ctx(), alice(), QString()); // Alice is in Bob's book
        const qint64 id = aliceWrites(QStringLiteral("minutes"), true);
        QVERIFY(id);
        QCOMPARE(mail::Search::detail(m_bob->ctx(), id)->receiptTo, alice());
        // Fetching is not reading.
        QCOMPARE(receiptsQueued(m_bob), 0);
        bobReads(id);
        QCOMPARE(receiptsQueued(m_bob), 1);
        const auto sent = mail::Search::detail(m_alice->ctx(), sentId(QStringLiteral("minutes")));
        QCOMPARE(sent->readBy.size(), 1);
        QCOMPARE(sent->readBy[0].addr, bob());
        // The receipt is not mail to read.
        QCOMPARE(mail::Search::count(m_alice->ctx(), tag::Inbox, false), 0);
        QCOMPARE(mail::Search::count(m_alice->ctx(), tag::Unverified, false), 0);
        // Reading again sends nothing more.
        mail::Organize::markRead(m_bob->ctx(), id, false);
        mail::Organize::markRead(m_bob->ctx(), id, true);
        QCOMPARE(receiptsQueued(m_bob), 1);
        QCOMPARE(m_bob->ctx().db.queryInt("SELECT count(*) FROM ops WHERE kind='mdn.sent'").value_or(0), 1);
    }

    void encryptedMailGetsAnEncryptedReceipt()
    {
        mail::Contacts::create(m_bob->ctx(), alice(), QString());
        aliceWrites(QStringLiteral("hello"), false); // Bob learns Alice's key
        Draft d;
        d.to = {{QString(), alice()}};
        d.subject = QStringLiteral("hi back");
        d.text = QStringLiteral("hi");
        mail::Compose::queue(m_bob->ctx(), d); // Alice learns Bob's
        sync(m_bob);
        sync(m_alice);
        const qint64 id = aliceWrites(QStringLiteral("secret"), true);
        QVERIFY(mail::Search::detail(m_bob->ctx(), id)->encrypted);
        const int before = int(m_srv->envelopes().size());
        bobReads(id);
        QCOMPARE(m_srv->envelopes().size(), before + 1);
        const QByteArray wire = m_srv->envelopes().last().data;
        QVERIFY(wire.contains("multipart/encrypted"));
        QVERIFY(!wire.contains("disposition-notification"));
        QCOMPARE(mail::Search::detail(m_alice->ctx(), sentId(QStringLiteral("secret")))->readBy.size(), 1);
    }

    void noReceiptUnlessTrustedAndAllowed()
    {
        // A stranger to Bob: held, and no receipt.
        const qint64 held = aliceWrites(QStringLiteral("cold"), true);
        QVERIFY(mail::Search::detail(m_bob->ctx(), held)->held);
        mail::Organize::markRead(m_bob->ctx(), held, true);
        QCOMPARE(receiptsQueued(m_bob), 0);

        // In the book, but trust taken away by hand: none.
        mail::Contacts::create(m_bob->ctx(), alice(), QString());
        mail::Contacts::setTrust(m_bob->ctx(), alice(), false);
        const qint64 untrusted = aliceWrites(QStringLiteral("untrusted"), true);
        mail::Organize::markRead(m_bob->ctx(), untrusted, true);
        QCOMPARE(receiptsQueued(m_bob), 0);

        // Trusted, but the setting says never: none.
        mail::Contacts::setTrust(m_bob->ctx(), alice(), std::nullopt);
        mail::Preferences::set(m_bob->ctx(), cfg::MdnSend, QStringLiteral("never"));
        const qint64 never = aliceWrites(QStringLiteral("never"), true);
        mail::Organize::markRead(m_bob->ctx(), never, true);
        QCOMPARE(receiptsQueued(m_bob), 0);

        // Asked to send it somewhere other than the sender: none.
        mail::Preferences::set(m_bob->ctx(), cfg::MdnSend, QStringLiteral("trusted"));
        m_srv->deliver(bob(), "From: alice@x.test\r\nTo: bob@x.test\r\nSubject: elsewhere\r\n"
                              "Message-ID: <elsewhere@x.test>\r\nDisposition-Notification-To: spy@y.test\r\n\r\nhi\r\n");
        sync(m_bob);
        const auto elsewhere = m_bob->ctx().db.queryInt("SELECT id FROM messages WHERE message_id='elsewhere@x.test'");
        QVERIFY(elsewhere);
        mail::Organize::markRead(m_bob->ctx(), *elsewhere, true);
        QCOMPARE(receiptsQueued(m_bob), 0);

        // Strangers' mail not held (gating off): still no receipt to them.
        mail::Preferences::set(m_bob->ctx(), cfg::Gating, QStringLiteral("0"));
        mail::Contacts::setTrust(m_bob->ctx(), alice(), false);
        const qint64 open = aliceWrites(QStringLiteral("not held"), true);
        QVERIFY(!mail::Search::detail(m_bob->ctx(), open)->held);
        mail::Organize::markRead(m_bob->ctx(), open, true);
        QCOMPARE(receiptsQueued(m_bob), 0);
        mail::Contacts::setTrust(m_bob->ctx(), alice(), std::nullopt);

        // And all along, a message that asked nothing sends nothing.
        const qint64 plain = aliceWrites(QStringLiteral("plain"), false);
        mail::Organize::markRead(m_bob->ctx(), plain, true);
        QCOMPARE(receiptsQueued(m_bob), 0);
    }

    // A receipt from someone the message did not go to is ignored.
    void forgedReceiptIgnored()
    {
        aliceWrites(QStringLiteral("to bob only"), true);
        const QString mid = mail::Organize::messageIdOf(m_alice->ctx(), sentId(QStringLiteral("to bob only")));
        m_srv->deliver(alice(), "From: carol@x.test\r\nTo: alice@x.test\r\nSubject: Read\r\nMessage-ID: <f@x.test>\r\n"
                                "Content-Type: multipart/report; report-type=disposition-notification; boundary=b\r\n\r\n"
                                "--b\r\nContent-Type: text/plain\r\n\r\nread\r\n--b\r\n"
                                "Content-Type: message/disposition-notification\r\n\r\nOriginal-Message-ID: <"
                                    + mid.toUtf8() + ">\r\nDisposition: manual-action/MDN-sent-manually; displayed\r\n--b--\r\n");
        sync(m_alice);
        QVERIFY(mail::Search::detail(m_alice->ctx(), sentId(QStringLiteral("to bob only")))->readBy.isEmpty());
        // Hidden all the same.
        QCOMPARE(mail::Search::count(m_alice->ctx(), tag::Unverified, false), 0);
    }

    // Read on one device: the other sends no second receipt.
    void oneReceiptForAllDevices()
    {
        mail::Contacts::create(m_bob->ctx(), alice(), QString());
        const qint64 id = aliceWrites(QStringLiteral("twice"), true);
        const QString root = m_dir->path() + QStringLiteral("/bob2");
        QVERIFY(cloneDevice(m_bob, root));
        m_mgr2 = std::make_unique<AccountManager>(root);
        Account *bob2 = m_mgr2->account(m_bob->id());
        mail::Organize::markRead(m_bob->ctx(), id, true);
        QCOMPARE(receiptsQueued(m_bob), 1);
        // The op reaches the second device, which then reads it too.
        QList<Op> ops;
        Statement st(m_bob->ctx().db, "SELECT device, seq, hlc, kind, payload FROM ops WHERE device=? ORDER BY seq");
        st.bind(1, m_bob->ctx().ops.deviceId());
        while (st.step())
            ops.append({st.text(0), st.int64(1), st.int64(2), st.text(3),
                        QJsonDocument::fromJson(st.text(4).toUtf8()).object()});
        sync::OpApply::apply(bob2->ctx(), ops);
        mail::Organize::markRead(bob2->ctx(), id, false);
        mail::Organize::markRead(bob2->ctx(), id, true);
        QCOMPARE(receiptsQueued(bob2), 0);
    }

    void disappearsAfterReading()
    {
        mail::Contacts::create(m_bob->ctx(), alice(), QString());
        const qint64 id = aliceWrites(QStringLiteral("brief"), false, 3600);
        auto d = mail::Search::detail(m_bob->ctx(), id);
        QCOMPARE(d->ephemeralSeconds, 3600);
        QVERIFY(!d->expiresAt.isValid()); // unread: the clock has not started
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        QCOMPARE(mail::Organize::expireDue(m_bob->ctx(), now + 10LL * 24 * 3600 * 1000), 0);

        mail::Organize::markRead(m_bob->ctx(), id, true);
        d = mail::Search::detail(m_bob->ctx(), id);
        QVERIFY(qAbs(d->expiresAt.toMSecsSinceEpoch() - (now + 3600 * 1000)) < 60 * 1000);
        QCOMPARE(mail::Organize::nextExpiry(m_bob->ctx()).value_or(0), d->expiresAt.toMSecsSinceEpoch());
        QCOMPARE(mail::Organize::expireDue(m_bob->ctx(), now + 1000), 0);
        QCOMPARE(mail::Organize::expireDue(m_bob->ctx(), now + 3601 * 1000), 1);
        d = mail::Search::detail(m_bob->ctx(), id);
        QVERIFY(d->trashed);
        QCOMPARE(d->trashReason, TrashReason::Expired);
        // What happened travels as an op, so the other devices follow.
        QCOMPARE(m_bob->ctx().db.queryInt("SELECT count(*) FROM ops WHERE kind='msg.trash'").value_or(0), 1);
        QVERIFY(!mail::Organize::nextExpiry(m_bob->ctx()));

        // Alice's own copy started its clock when it was sent.
        const auto sent = mail::Search::detail(m_alice->ctx(), sentId(QStringLiteral("brief")));
        QCOMPARE(sent->ephemeralSeconds, 3600);
        QVERIFY(sent->expiresAt.isValid());
        QVERIFY(qAbs(sent->expiresAt.toMSecsSinceEpoch() - (now + 3600 * 1000)) < 60 * 1000);
    }

    // Read on another device: the clock runs from then, here too.
    void readElsewhereStartsTheClock()
    {
        const qint64 id = aliceWrites(QStringLiteral("elsewhere"), false, 600);
        const QString mid = mail::Organize::messageIdOf(m_bob->ctx(), id);
        const qint64 readAt = QDateTime::currentMSecsSinceEpoch() - 100 * 1000;
        Op o{QStringLiteral("other-device"), 1, readAt << 16, op::ReadState,
             {{QStringLiteral("mid"), mid}, {QStringLiteral("read"), true}}};
        sync::OpApply::apply(m_bob->ctx(), {o});
        const auto d = mail::Search::detail(m_bob->ctx(), id);
        QVERIFY(!d->unread);
        QCOMPARE(d->expiresAt.toMSecsSinceEpoch(), readAt + 600 * 1000);
    }

    void trustSetByHand()
    {
        // A stranger's mail waits; trusting them releases it and the next.
        const qint64 held = aliceWrites(QStringLiteral("one"), false);
        QVERIFY(mail::Search::detail(m_bob->ctx(), held)->held);
        mail::Contacts::setTrust(m_bob->ctx(), alice(), true);
        QVERIFY(!mail::Search::detail(m_bob->ctx(), held)->held);
        QVERIFY(mail::Contacts::isTrusted(m_bob->ctx(), alice()));
        QVERIFY(!mail::Search::detail(m_bob->ctx(), aliceWrites(QStringLiteral("two"), false))->held);

        // Someone in the book, untrusted by hand: their mail waits again.
        mail::Contacts::create(m_bob->ctx(), alice(), QString());
        mail::Contacts::setTrust(m_bob->ctx(), alice(), false);
        QVERIFY(!mail::Contacts::isTrusted(m_bob->ctx(), alice()));
        QVERIFY(mail::Search::detail(m_bob->ctx(), aliceWrites(QStringLiteral("three"), false))->held);

        // Back to the rule: in the book, so trusted.
        mail::Contacts::setTrust(m_bob->ctx(), alice(), std::nullopt);
        QVERIFY(mail::Contacts::isTrusted(m_bob->ctx(), alice()));
        QCOMPARE(m_bob->ctx().db.queryInt("SELECT count(*) FROM ops WHERE kind='contact.trust'").value_or(0), 3);

        // Ourselves, always.
        mail::Contacts::setTrust(m_bob->ctx(), bob(), false);
        QVERIFY(mail::Contacts::isTrusted(m_bob->ctx(), bob()));
    }
};

QTEST_GUILESS_MAIN(TestReceipts)
#include "tst_receipts.moc"
