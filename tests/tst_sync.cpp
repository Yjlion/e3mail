// SPDX-License-Identifier: MPL-2.0
#include "FakeMailServer.h"
#include "LoopbackTransport.h"
#include "engine/AccountManager.h"
#include "mail/Compose.h"
#include "mail/Contacts.h"
#include "mail/Organize.h"
#include "mail/Preferences.h"
#include "mail/Search.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"
#include "sync/OpApply.h"
#include "sync/SyncSession.h"

#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace e3;

// Two devices of one account (alice), plus bob who writes to her, against
// the in-process mail system and a loopback network between the devices.
// Device B starts as a copy of device A with its own device id, which is
// what pairing will hand over.
class TestSync : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<FakeMailServer> m_srv;
    std::unique_ptr<AccountManager> m_mgrA;
    std::unique_ptr<AccountManager> m_mgrB;
    std::unique_ptr<LoopbackNetwork> m_net;
    std::unique_ptr<sync::SyncSession> m_syncA;
    std::unique_ptr<sync::SyncSession> m_syncB;
    Account *m_a = nullptr;
    Account *m_b = nullptr;
    Account *m_bob = nullptr;

    static const QString alice() { return QStringLiteral("alice@x.test"); }
    static const QString bob() { return QStringLiteral("bob@x.test"); }

    Account *makeAccount(AccountManager &mgr, const QString &addr)
    {
        m_srv->addUser(addr, QStringLiteral("pw"));
        Account *a = mgr.create();
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

    static void fetch(Account *a) { MailWorker::runOnce(a->dir(), a->credentials()); }

    // A second device: the account directory copied, with a new device id.
    Account *cloneDevice(Account *src)
    {
        src->ctx().db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
        const QString root = m_dir->path() + QStringLiteral("/deviceB");
        const QString dst = root + QStringLiteral("/accounts/") + QString::number(src->id());
        QDirIterator it(src->dir(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString from = it.next();
            const QString to = dst + from.mid(src->dir().size());
            QDir().mkpath(QFileInfo(to).path());
            if (!QFile::copy(from, to))
                return nullptr;
        }
        {
            Database db(dst + QStringLiteral("/mail.db"));
            db.run("UPDATE config SET value=? WHERE key='device_id'",
                   QUuid::createUuid().toString(QUuid::WithoutBraces));
        }
        QFile f(root + QStringLiteral("/accounts/accounts.json"));
        if (!f.open(QIODevice::WriteOnly))
            return nullptr;
        f.write(QJsonDocument(QJsonObject{{QStringLiteral("accounts"), QJsonArray{src->id()}},
                                          {QStringLiteral("next_id"), src->id() + 1}})
                    .toJson());
        f.close();
        m_mgrB = std::make_unique<AccountManager>(root);
        return m_mgrB->account(src->id());
    }

    static QString device(Account *a) { return a->ctx().ops.deviceId(); }

    void connectDevices()
    {
        m_net->link(device(m_a), device(m_b));
        m_net->settle();
    }

    static qint64 idOf(Account *a, const QString &mid)
    {
        return a->ctx().db.queryInt("SELECT id FROM messages WHERE message_id=?", mid).value_or(0);
    }

    static QString midOf(Account *a, qint64 id) { return mail::Organize::messageIdOf(a->ctx(), id); }

    void bobWrites(const QString &subject)
    {
        Draft d;
        d.to = {{QString(), alice()}};
        d.subject = subject;
        d.text = QStringLiteral("text of ") + subject;
        mail::Compose::queue(m_bob->ctx(), d);
        fetch(m_bob);
    }

    QList<MessageSummary> inbox(Account *a) { return mail::Search::list(a->ctx(), SearchQuery::forTag(tag::Inbox)); }

    static qint64 opCount(Account *a, const QString &kind)
    {
        return a->ctx().db.queryInt("SELECT count(*) FROM ops WHERE kind=? AND device=?", kind, device(a)).value_or(0);
    }

private Q_SLOTS:
    void init()
    {
        qputenv("E3MAIL_NO_KEYCHAIN", "1");
        m_dir = std::make_unique<QTemporaryDir>();
        m_srv = std::make_unique<FakeMailServer>();
        m_mgrA = std::make_unique<AccountManager>(m_dir->path() + QStringLiteral("/deviceA"));
        m_a = makeAccount(*m_mgrA, alice());
        m_bob = makeAccount(*m_mgrA, bob());
        // Alice knows Bob, so his mail lands in the Inbox, and Bob has
        // Alice's key from an earlier exchange, so what he writes is encrypted.
        mail::Organize::accept(m_a->ctx(), bob());
        Draft hello;
        hello.to = {{QString(), bob()}};
        hello.subject = QStringLiteral("hello");
        hello.text = QStringLiteral("hello");
        mail::Compose::queue(m_a->ctx(), hello);
        fetch(m_a);
        fetch(m_bob);

        m_b = cloneDevice(m_a);
        QVERIFY(m_b);
        QVERIFY(device(m_a) != device(m_b));
        sync::SyncSession::pair(m_a->ctx(), device(m_b), QStringLiteral("B"));
        sync::SyncSession::pair(m_b->ctx(), device(m_a), QStringLiteral("A"));

        m_net = std::make_unique<LoopbackNetwork>();
        m_syncA = std::make_unique<sync::SyncSession>(m_a->ctx(), *m_net->endpoint(device(m_a)));
        m_syncB = std::make_unique<sync::SyncSession>(m_b->ctx(), *m_net->endpoint(device(m_b)));
    }

    void cleanup()
    {
        m_syncA.reset();
        m_syncB.reset();
        m_net.reset();
        m_mgrB.reset();
        m_mgrA.reset();
        m_srv.reset();
        m_dir.reset();
    }

    // The server keeps a message until both devices have it, then the next
    // device to look deletes it.
    void serverDeletionWaitsForEveryDevice()
    {
        bobWrites(QStringLiteral("one"));
        bobWrites(QStringLiteral("two"));
        fetch(m_a);
        QCOMPARE(inbox(m_a).size(), 2);
        QCOMPARE(m_srv->mailbox(alice()).size(), 2); // B has not acknowledged

        fetch(m_b);
        QCOMPARE(inbox(m_b).size(), 2);
        QCOMPARE(m_srv->mailbox(alice()).size(), 2); // B has not heard from A

        connectDevices();
        fetch(m_a);
        QCOMPARE(m_srv->mailbox(alice()).size(), 0);

        // Unpaired, the single-device rule is back.
        sync::SyncSession::unpair(m_a->ctx(), device(m_b));
        bobWrites(QStringLiteral("three"));
        fetch(m_a);
        QCOMPARE(m_srv->mailbox(alice()).size(), 0);
    }

    // What a person does on one device shows on the other, whether the other
    // has the message from the server already or not.
    void changesTravel()
    {
        bobWrites(QStringLiteral("one"));
        bobWrites(QStringLiteral("two"));
        fetch(m_a);
        const auto list = inbox(m_a);
        QCOMPARE(list.size(), 2);
        const QString one = midOf(m_a, list[0].id);
        const QString two = midOf(m_a, list[1].id);
        auto &a = m_a->ctx();
        mail::Organize::markRead(a, list[0].id, true);
        const qint64 work = mail::Organize::createLabel(a, QStringLiteral("Work"), QStringLiteral("#1971c2"));
        mail::Organize::setLabel(a, list[0].id, work, true);
        mail::Organize::trash(a, list[1].id, TrashReason::User);
        mail::Contacts::block(a, QStringLiteral("spam@z.test"));
        mail::Preferences::set(a, cfg::TrashPurgeDays, QStringLiteral("7"));
        mail::Preferences::set(a, cfg::PollSeconds, QStringLiteral("600")); // stays on A

        connectDevices();
        auto &b = m_b->ctx();
        // B had neither message: it fetched both from A, then applied what A did.
        const auto one_b = mail::Search::detail(b, idOf(m_b, one));
        QVERIFY(one_b);
        QVERIFY(!one_b->unread);
        QCOMPARE(one_b->labels.value(0).name, QStringLiteral("Work"));
        QCOMPARE(one_b->bodyText, QStringLiteral("text of ") + one_b->subject); // decrypted with the shared key
        QVERIFY(one_b->encrypted);
        QVERIFY(mail::Search::detail(b, idOf(m_b, two))->trashed);
        QVERIFY(mail::Contacts::isBlocked(b, QStringLiteral("spam@z.test")));
        QCOMPARE(m_b->ctx().config.getInt(cfg::TrashPurgeDays), 7);
        QVERIFY(m_b->ctx().config.getInt(cfg::PollSeconds) != 600);
        QCOMPARE(mail::Organize::labels(b).value(0).color, QStringLiteral("#1971c2"));

        // And back: B restores, renames the label; A follows, live.
        mail::Organize::restore(b, idOf(m_b, two));
        mail::Organize::renameLabel(b, mail::Organize::labels(b).value(0).id, QStringLiteral("Job"), QString());
        m_syncB->push();
        m_net->settle();
        QVERIFY(!mail::Search::detail(a, idOf(m_a, two))->trashed);
        QCOMPARE(mail::Organize::labels(a).size(), 1);
        QCOMPARE(mail::Organize::labels(a).value(0).name, QStringLiteral("Job"));

        // Mail Alice sends from A appears in B's Sent, though the server
        // never holds a copy of it.
        Draft d;
        d.to = {{QString(), bob()}};
        d.subject = QStringLiteral("from A");
        d.text = QStringLiteral("sent from A");
        const QString sent = midOf(m_a, mail::Compose::queue(a, d));
        fetch(m_a);
        m_syncA->push();
        m_net->settle();
        const auto sent_b = mail::Search::detail(b, idOf(m_b, sent));
        QVERIFY(sent_b);
        QCOMPARE(sent_b->direction, Direction::Outgoing);
        QCOMPARE(sent_b->bodyText.trimmed(), QStringLiteral("sent from A"));
    }

    // An op for a message the device does not have yet waits in the log and
    // applies when the message arrives from the server.
    void opsWaitForTheirMessage()
    {
        m_net->withholdBlobs = true;
        bobWrites(QStringLiteral("late"));
        fetch(m_a);
        const qint64 id = inbox(m_a).first().id;
        const QString mid = midOf(m_a, id);
        mail::Organize::markRead(m_a->ctx(), id, true);
        mail::Organize::archive(m_a->ctx(), id, true);

        connectDevices();
        QCOMPARE(idOf(m_b, mid), 0);
        fetch(m_b); // the server still has it: B had not acknowledged it
        const auto d = mail::Search::detail(m_b->ctx(), idOf(m_b, mid));
        QVERIFY(d);
        QVERIFY(!d->unread);
        QVERIFY(d->archived);
    }

    // A purge on one device is final everywhere, including against the
    // server copy the other device has not fetched yet.
    void purgeIsFinal()
    {
        bobWrites(QStringLiteral("doomed"));
        fetch(m_a);
        const qint64 id = inbox(m_a).first().id;
        const QString mid = midOf(m_a, id);
        mail::Organize::trash(m_a->ctx(), id, TrashReason::User);
        mail::Organize::emptyTrash(m_a->ctx());

        connectDevices();
        QCOMPARE(idOf(m_b, mid), 0);
        fetch(m_b);
        QCOMPARE(idOf(m_b, mid), 0);
        // B acknowledged it all the same, so the server copy can go.
        fetch(m_a);
        QCOMPARE(m_srv->mailbox(alice()).size(), 0);
    }

    // Both devices change the same thing while apart: the later change wins
    // on both, whichever order the ops arrive in.
    void lastWriterWins()
    {
        bobWrites(QStringLiteral("contested"));
        fetch(m_a);
        fetch(m_b);
        const QString mid = midOf(m_a, inbox(m_a).first().id);
        mail::Organize::markRead(m_a->ctx(), idOf(m_a, mid), true);
        QTest::qWait(5);
        mail::Organize::markRead(m_b->ctx(), idOf(m_b, mid), true);
        mail::Organize::markRead(m_b->ctx(), idOf(m_b, mid), false);
        QTest::qWait(5);
        const qint64 l = mail::Organize::createLabel(m_a->ctx(), QStringLiteral("Later"), QString());
        mail::Organize::setLabel(m_a->ctx(), idOf(m_a, mid), l, true);

        connectDevices();
        for (Account *x : {m_a, m_b}) {
            const auto d = mail::Search::detail(x->ctx(), idOf(x, mid));
            QVERIFY(d->unread);
            QCOMPARE(d->labels.value(0).name, QStringLiteral("Later"));
        }
    }

    // Equal clocks: the device id breaks the tie, the same way everywhere.
    void tieBreaksByDevice()
    {
        auto &ctx = m_a->ctx();
        const QJsonObject on{{QStringLiteral("pattern"), QStringLiteral("x@z.test")}};
        const qint64 hlc = qint64(1) << 50;
        const QList<Op> ops = {{QStringLiteral("dev-z"), 1, hlc, op::Block, on},
                               {QStringLiteral("dev-a"), 1, hlc, op::Unblock, on}};
        sync::OpApply::apply(ctx, ops);
        QVERIFY(mail::Contacts::isBlocked(ctx, QStringLiteral("x@z.test")));
        // Replaying is harmless.
        QCOMPARE(sync::OpApply::apply(ctx, ops).accepted, 0);
        // And out-of-sequence ops are refused, not applied.
        const auto r = sync::OpApply::apply(ctx, {{QStringLiteral("dev-a"), 3, hlc + 1, op::Unblock, on}});
        QVERIFY(r.gap);
        QCOMPARE(r.accepted, 0);
        QVERIFY(mail::Contacts::isBlocked(ctx, QStringLiteral("x@z.test")));
    }

    // A lost message leaves a gap; the next one reveals it and the peer
    // resends from where the gap began.
    void gapIsRefilled()
    {
        connectDevices();
        auto &a = m_a->ctx();
        m_net->drop(device(m_a), device(m_b), 1);
        mail::Contacts::block(a, QStringLiteral("one@z.test"));
        m_syncA->push();
        m_net->settle();
        QVERIFY(!mail::Contacts::isBlocked(m_b->ctx(), QStringLiteral("one@z.test")));
        mail::Contacts::block(a, QStringLiteral("two@z.test"));
        m_syncA->push();
        m_net->settle();
        QVERIFY(mail::Contacts::isBlocked(m_b->ctx(), QStringLiteral("one@z.test")));
        QVERIFY(mail::Contacts::isBlocked(m_b->ctx(), QStringLiteral("two@z.test")));
    }

    // A third device sees A's changes through B, never having met A.
    void opsRelay()
    {
        auto &a = m_a->ctx();
        connectDevices();
        mail::Contacts::block(a, QStringLiteral("relay@z.test"));
        m_syncA->push();
        m_net->settle();
        // B now holds A's op and offers it to anyone who lacks it.
        const auto missing = sync::OpApply::missing(m_b->ctx().db, {}, 10000);
        bool found = false;
        for (const Op &o : missing)
            found |= o.device == device(m_a) && o.kind == op::Block
                && o.payload.value(QLatin1String("pattern")).toString() == QLatin1String("relay@z.test");
        QVERIFY(found);
    }

    // A device that is not paired is not listened to.
    void strangersAreIgnored()
    {
        sync::SyncSession::unpair(m_b->ctx(), device(m_a));
        connectDevices();
        mail::Contacts::block(m_a->ctx(), QStringLiteral("x@z.test"));
        m_syncA->push();
        m_net->settle();
        QVERIFY(!mail::Contacts::isBlocked(m_b->ctx(), QStringLiteral("x@z.test")));
    }

    // Every user-visible change is recorded, including those that once were
    // not.
    void everyChangeIsRecorded()
    {
        auto &a = m_a->ctx();
        const qint64 l = mail::Organize::createLabel(a, QStringLiteral("L"), QString());
        mail::Organize::renameLabel(a, l, QStringLiteral("M"), QString());
        QCOMPARE(opCount(m_a, op::LabelRename), 1);
        const qint64 c = mail::Contacts::touch(a, QStringLiteral("p@z.test"), QString(), ContactOrigin::Manual);
        mail::Contacts::setEncryptionOverride(a, c, EncryptionMode::Strict);
        mail::Contacts::setEncryptionOverride(a, c, std::nullopt);
        QCOMPARE(opCount(m_a, op::ContactPolicy), 2);
        mail::Contacts::setVerified(a, QStringLiteral("p@z.test"), QStringLiteral("ABCD"));
        QCOMPARE(opCount(m_a, op::ContactVerify), 1);
        mail::Preferences::set(a, cfg::Signature, QStringLiteral("-- A"));
        QCOMPARE(opCount(m_a, op::Setting), 1);
        bobWrites(QStringLiteral("ack me"));
        fetch(m_a);
        QCOMPARE(opCount(m_a, op::ServerAck), 1);
    }
};

QTEST_GUILESS_MAIN(TestSync)
#include "tst_sync.moc"
