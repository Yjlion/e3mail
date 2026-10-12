// SPDX-License-Identifier: MPL-2.0
// Two devices of one account syncing over real Iroh endpoints on this
// machine, with the relay off: the transport under SyncSession, where
// tst_sync uses a loopback. Pairing is the clone tst_sync uses too, plus each
// device recording the other's endpoint.
#include "DeviceClone.h"
#include "FakeMailServer.h"
#include "engine/AccountManager.h"
#include "engine/Pairing.h"
#include "mail/Compose.h"
#include "mail/Contacts.h"
#include "mail/Organize.h"
#include "mail/Search.h"
#include "store/BlobStore.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"
#include "sync/IrohTransport.h"

#include <QDateTime>
#include <QTemporaryDir>
#include <QtTest>

using namespace e3;

class TestP2p : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<FakeMailServer> m_srv;
    std::unique_ptr<AccountManager> m_mgrA, m_mgrB;
    Account *m_a = nullptr;
    Account *m_b = nullptr;
    Account *m_bob = nullptr;

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
        // No relay and no address lookup reach outside this machine in tests.
        a->ctx().config.set(cfg::P2pRelay, QStringLiteral("off"));
        a->ctx().config.set(cfg::P2pLoopback, QStringLiteral("1"));
        return a;
    }

    static void fetch(Account *a) { MailWorker::runOnce(a->dir(), a->credentials()); }
    static QString device(Account *a) { return a->ctx().ops.deviceId(); }
    static sync::IrohTransport *iroh(Account *a) { return qobject_cast<sync::IrohTransport *>(a->deviceTransport()); }

    // What pairing ends with: each device knows the other's endpoint.
    static void introduce(Account *to, Account *other, const QString &name)
    {
        to->ctx().db.run("INSERT OR REPLACE INTO devices(id, name, paired_at, node_id, addr) VALUES(?, ?, ?, ?, ?)",
                         device(other), name, QDateTime::currentMSecsSinceEpoch(), iroh(other)->nodeId(),
                         iroh(other)->addr());
        iroh(to)->refreshPeers();
    }

    void bobWrites(const QString &subject)
    {
        Draft d;
        d.to = {{QString(), QStringLiteral("alice@x.test")}};
        d.subject = subject;
        d.text = QStringLiteral("text of ") + subject;
        mail::Compose::queue(m_bob->ctx(), d);
        fetch(m_bob);
    }

    static qint64 idOf(Account *a, const QString &mid)
    {
        return a->ctx().db.queryInt("SELECT id FROM messages WHERE message_id=?", mid).value_or(0);
    }

private Q_SLOTS:
    void init()
    {
        qputenv("E3MAIL_NO_KEYCHAIN", "1");
        m_dir = std::make_unique<QTemporaryDir>();
        m_srv = std::make_unique<FakeMailServer>();
        m_mgrA = std::make_unique<AccountManager>(m_dir->path() + QStringLiteral("/A"));
        m_a = makeAccount(*m_mgrA, QStringLiteral("alice@x.test"));
        m_bob = makeAccount(*m_mgrA, QStringLiteral("bob@x.test"));
        mail::Organize::accept(m_a->ctx(), QStringLiteral("bob@x.test"));
        fetch(m_a);
        QVERIFY(cloneDevice(m_a, m_dir->path() + QStringLiteral("/B")));
        m_mgrB = std::make_unique<AccountManager>(m_dir->path() + QStringLiteral("/B"));
        m_b = m_mgrB->account(m_a->id());
        QVERIFY(m_b);
        QVERIFY(m_a->startDevices());
        QVERIFY(m_b->startDevices());
        QVERIFY(iroh(m_a)->nodeId() != iroh(m_b)->nodeId());
        introduce(m_a, m_b, QStringLiteral("B"));
        introduce(m_b, m_a, QStringLiteral("A"));
    }

    void cleanup()
    {
        m_mgrB.reset();
        m_mgrA.reset();
        m_srv.reset();
        m_dir.reset();
    }

    void devicesConnectAndChangesTravel()
    {
        bobWrites(QStringLiteral("over iroh"));
        fetch(m_a);
        const auto list = mail::Search::list(m_a->ctx(), SearchQuery::forTag(tag::Inbox));
        QCOMPARE(list.size(), 1);
        const QString mid = mail::Organize::messageIdOf(m_a->ctx(), list[0].id);
        mail::Organize::markRead(m_a->ctx(), list[0].id, true);

        iroh(m_a)->dialPaired();
        QTRY_VERIFY_WITH_TIMEOUT(iroh(m_a)->isConnected(device(m_b)) && iroh(m_b)->isConnected(device(m_a)), 15000);
        // B never fetched it: the raw message came from A, then A's read.
        QTRY_VERIFY_WITH_TIMEOUT(idOf(m_b, mid) != 0, 15000);
        const auto onB = mail::Search::detail(m_b->ctx(), idOf(m_b, mid));
        QCOMPARE(onB->bodyText.trimmed(), QStringLiteral("text of over iroh"));
        QTRY_VERIFY_WITH_TIMEOUT(!mail::Search::detail(m_b->ctx(), idOf(m_b, mid))->unread, 15000);

        // And back, live: B archives, A follows.
        mail::Organize::archive(m_b->ctx(), idOf(m_b, mid), true);
        m_b->notifyChanged();
        QTRY_COMPARE_WITH_TIMEOUT(mail::Search::count(m_a->ctx(), tag::Inbox, false), 0, 15000);
    }

    // An endpoint that is no paired device's gets nowhere.
    void strangersAreRefused()
    {
        QVERIFY(m_bob->startDevices());
        iroh(m_bob)->dial(iroh(m_a)->addr());
        QSignalSpy strangers(iroh(m_a), &sync::IrohTransport::strangerConnected);
        QTest::qWait(1500);
        QCOMPARE(strangers.size(), 0);
        QVERIFY(iroh(m_a)->connectedDevices().isEmpty());
    }

    // A device that goes away and comes back is dialed again.
    void reconnects()
    {
        iroh(m_a)->dialPaired();
        QTRY_VERIFY_WITH_TIMEOUT(iroh(m_a)->isConnected(device(m_b)), 15000);
        m_b->stopDevices();
        QTRY_VERIFY_WITH_TIMEOUT(!iroh(m_a)->isConnected(device(m_b)), 15000);
        QVERIFY(m_b->startDevices()); // dials A where A was last seen
        QTRY_VERIFY_WITH_TIMEOUT(iroh(m_a)->isConnected(device(m_b)), 15000);
        mail::Contacts::block(m_a->ctx(), QStringLiteral("spam@z.test"));
        m_a->notifyChanged();
        QTRY_VERIFY_WITH_TIMEOUT(m_b->ctx().db.queryInt("SELECT count(*) FROM blocklist").value_or(0) == 1, 15000);
    }

    // Both dial at once: they settle on one connection, and nothing sent on
    // the one they drop is lost.
    void simultaneousDials()
    {
        for (int round = 0; round < 5; ++round) {
            iroh(m_a)->dialPaired();
            iroh(m_b)->dialPaired();
            QTRY_VERIFY_WITH_TIMEOUT(iroh(m_a)->isConnected(device(m_b)) && iroh(m_b)->isConnected(device(m_a)), 15000);
            const QString pattern = QStringLiteral("round%1@z.test").arg(round);
            mail::Contacts::block(m_a->ctx(), pattern);
            m_a->notifyChanged();
            QTRY_VERIFY_WITH_TIMEOUT(mail::Contacts::isBlocked(m_b->ctx(), pattern), 15000);
            m_b->stopDevices();
            QTRY_VERIFY_WITH_TIMEOUT(!iroh(m_a)->isConnected(device(m_b)), 15000);
            QVERIFY(m_b->startDevices());
        }
    }

    // A removed device is dropped at once and refused from then on.
    void removedDeviceIsRefused()
    {
        iroh(m_a)->dialPaired();
        QTRY_VERIFY_WITH_TIMEOUT(iroh(m_a)->isConnected(device(m_b)), 15000);
        m_a->removeDevice(device(m_b));
        QTRY_VERIFY_WITH_TIMEOUT(!iroh(m_b)->isConnected(device(m_a)), 15000);
        QCOMPARE(m_a->ctx().db.queryInt("SELECT count(*) FROM ops WHERE kind='device.unpair'").value_or(0), 1);
        iroh(m_b)->dialPaired();
        QTest::qWait(1500);
        QVERIFY(!iroh(m_a)->isConnected(device(m_b)));
        QVERIFY(!iroh(m_b)->isConnected(device(m_a)));
    }

    // A new device joins with a code: the existing device allows it, the
    // new one pulls a copy, and from then on they sync.
    void pairing()
    {
        bobWrites(QStringLiteral("before pairing"));
        fetch(m_a);
        const QString mid = mail::Organize::messageIdOf(
            m_a->ctx(), mail::Search::list(m_a->ctx(), SearchQuery::forTag(tag::Inbox)).value(0).id);
        QVERIFY(!mid.isEmpty());
        iroh(m_a)->dialPaired();
        QTRY_VERIFY_WITH_TIMEOUT(iroh(m_b)->isConnected(device(m_a)), 15000);

        PairingOffer offer(*m_a);
        QVERIFY(offer.isReady());
        connect(&offer, &PairingOffer::requested, &offer, [&](const QString &name) {
            QCOMPARE(name, QStringLiteral("Laptop"));
            offer.allow();
        });
        QSignalSpy offered(&offer, &PairingOffer::finished);
        AccountManager mgrC(m_dir->path() + QStringLiteral("/C"));
        PairingJoin join(mgrC, offer.code(), QStringLiteral("Laptop"));
        QVERIFY(join.isValid());
        QCOMPARE(join.account(), QStringLiteral("alice@x.test"));
        QSignalSpy joined(&join, &PairingJoin::finished);
        QTRY_COMPARE_WITH_TIMEOUT(joined.size(), 1, 20000);
        QVERIFY2(joined.first().at(0).toBool(), qPrintable(joined.first().at(1).toString()));
        QTRY_COMPARE_WITH_TIMEOUT(offered.size(), 1, 5000);
        QVERIFY(offered.first().at(0).toBool());

        Account *c = mgrC.account(joined.first().at(2).toInt());
        QVERIFY(c);
        QCOMPARE(c->addr(), QStringLiteral("alice@x.test"));
        // Its own device, endpoint and name; the account's mail and passwords.
        QVERIFY(device(c) != device(m_a) && device(c) != device(m_b));
        QVERIFY(c->ctx().config.get(cfg::P2pSecret) != m_a->ctx().config.get(cfg::P2pSecret));
        QCOMPARE(c->ctx().config.get(cfg::DeviceName), QStringLiteral("Laptop"));
        QCOMPARE(c->settings().inPassword, QStringLiteral("pw"));
        QVERIFY(idOf(c, mid) != 0);
        QCOMPARE(mail::Search::detail(c->ctx(), idOf(c, mid))->bodyText.trimmed(), QStringLiteral("text of before pairing"));
        // It holds what the server holds, so the others need not wait for it.
        QVERIFY(c->ctx().db.queryInt("SELECT count(*) FROM ops WHERE kind='server.ack' AND device=?", device(c)).value_or(0) > 0);
        // A knows it, and told B.
        QVERIFY(m_a->ctx().db.queryInt("SELECT 1 FROM devices WHERE id=?", device(c)));
        QTRY_VERIFY_WITH_TIMEOUT(m_b->ctx().db.queryInt("SELECT 1 FROM devices WHERE id=?", device(c)).has_value(), 15000);

        // And they sync.
        QVERIFY(c->startDevices());
        QTRY_VERIFY_WITH_TIMEOUT(iroh(m_a)->isConnected(device(c)), 15000);
        mail::Organize::markRead(m_a->ctx(), idOf(m_a, mid), true);
        m_a->notifyChanged();
        QTRY_VERIFY_WITH_TIMEOUT(!mail::Search::detail(c->ctx(), idOf(c, mid))->unread, 15000);

        // The code was for one device.
        AccountManager mgrD(m_dir->path() + QStringLiteral("/D"));
        PairingJoin again(mgrD, offer.code(), QStringLiteral("Phone"));
        QSignalSpy refused(&again, &PairingJoin::finished);
        QTRY_COMPARE_WITH_TIMEOUT(refused.size(), 1, 15000);
        QVERIFY(!refused.first().at(0).toBool());
        QVERIFY(mgrD.accounts().isEmpty());
    }

    void pairingRefused()
    {
        PairingOffer offer(*m_a);
        connect(&offer, &PairingOffer::requested, &offer, [&] { offer.deny(); });
        AccountManager mgrC(m_dir->path() + QStringLiteral("/C"));
        PairingJoin join(mgrC, offer.code(), QStringLiteral("Laptop"));
        QSignalSpy joined(&join, &PairingJoin::finished);
        QTRY_COMPARE_WITH_TIMEOUT(joined.size(), 1, 15000);
        QVERIFY(!joined.first().at(0).toBool());
        QVERIFY(mgrC.accounts().isEmpty());
        QVERIFY(!m_a->ctx().db.queryInt("SELECT 1 FROM devices WHERE name='Laptop'"));

        // A wrong token never gets as far as asking.
        auto code = *pairing::parseCode(PairingOffer(*m_a).code());
        code.token[0] = char(code.token[0] ^ 1);
        PairingOffer listening(*m_a);
        QSignalSpy asked(&listening, &PairingOffer::requested);
        code.addr = pairing::parseCode(listening.code())->addr;
        PairingJoin wrong(mgrC, pairing::formatCode(code), QStringLiteral("Laptop"));
        QSignalSpy wrongDone(&wrong, &PairingJoin::finished);
        QTRY_COMPARE_WITH_TIMEOUT(wrongDone.size(), 1, 15000);
        QVERIFY(!wrongDone.first().at(0).toBool());
        QCOMPARE(asked.size(), 0);
    }

    // Every file is checked against its name, which is its SHA-256.
    void pairingRefusesADamagedFile()
    {
        bobWrites(QStringLiteral("damaged"));
        fetch(m_a);
        const QString blob = *m_a->ctx().db.queryText("SELECT raw_blob FROM messages WHERE raw_blob IS NOT NULL LIMIT 1");
        QFile f(m_a->ctx().blobs.path(blob));
        QVERIFY(f.open(QIODevice::Append));
        f.write("tampered");
        f.close();
        PairingOffer offer(*m_a);
        connect(&offer, &PairingOffer::requested, &offer, [&] { offer.allow(); });
        AccountManager mgrC(m_dir->path() + QStringLiteral("/C"));
        PairingJoin join(mgrC, offer.code(), QStringLiteral("Laptop"));
        QSignalSpy joined(&join, &PairingJoin::finished);
        QTRY_COMPARE_WITH_TIMEOUT(joined.size(), 1, 20000);
        QVERIFY(!joined.first().at(0).toBool());
        QVERIFY(mgrC.accounts().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestP2p)
#include "tst_p2p.moc"
