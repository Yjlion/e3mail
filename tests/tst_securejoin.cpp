// SPDX-License-Identifier: MPL-2.0
#include "DeviceClone.h"
#include "FakeMailServer.h"
#include "crypto/Autocrypt.h"
#include "crypto/Pgp.h"
#include "engine/AccountManager.h"
#include "engine/MailWorker.h"
#include "mail/Compose.h"
#include "mail/Contacts.h"
#include "mail/Keyring.h"
#include "mail/Organize.h"
#include "mail/Search.h"
#include "mail/SecureJoin.h"
#include "mime/Builder.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"

#include <QTemporaryDir>
#include <QTimeZone>
#include <QUuid>
#include <QtTest>

using namespace e3;
using mail::SecureJoin;

// SecureJoin's setup-contact protocol (ADR 0021) between real accounts over
// the in-process mail system: Alice shows an invite, Bob scans it.
class TestSecureJoin : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<FakeMailServer> m_srv;
    std::unique_ptr<AccountManager> m_mgr;
    std::unique_ptr<AccountManager> m_mgr2;

    static QString alice() { return QStringLiteral("alice@x.test"); }
    static QString bob() { return QStringLiteral("bob@x.test"); }
    static QString mallory() { return QStringLiteral("mallory@x.test"); }

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

    // Runs the handshake to its end: each side fetches and answers in turn.
    // A pass sends what is queued before it fetches, so an answer leaves on
    // the pass after the one that wrote it.
    static void exchange(Account *a, Account *b, int rounds = 7)
    {
        for (int i = 0; i < rounds; ++i) {
            sync(b);
            sync(a);
        }
    }

    static bool verified(Account *a, const QString &addr, const QString &fpr)
    {
        const auto c = mail::Contacts::get(a->ctx(), addr);
        return c && c->verified && c->fingerprint == fpr;
    }

    static QString fprOf(Account *a) { return mail::Keyring::selfFingerprint(a->ctx()); }

    static SecureJoin::JoinState joinState(Account *a, const QString &addr)
    {
        const auto j = SecureJoin::joinFor(a->ctx(), addr);
        return j ? j->state : SecureJoin::JoinState::None;
    }

    // Mallory writes to Alice encrypted and signed with her own key, naming
    // whatever she likes as the step and its headers.
    void forge(Account *from, Account *to, const QList<QPair<QByteArray, QByteArray>> &inner, bool encrypt = true,
               bool gossip = true)
    {
        auto &ctx = from->ctx();
        const QString self = ctx.config.get(cfg::Addr);
        const QString selfFpr = mail::Keyring::ensureSelfKey(ctx, self);
        const QString toAddr = to->ctx().config.get(cfg::Addr);
        mime::OutgoingMail m;
        m.from = {QString(), self};
        m.to = {{QString(), toAddr}};
        m.subject = QStringLiteral("Secure-Join");
        m.date = QDateTime::currentDateTimeUtc();
        m.messageId = QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral("@x.test");
        m.text = QStringLiteral("Secure-Join");
        m.extraHeaders.append(
            {"Autocrypt", crypto::Autocrypt::format(self, ctx.pgp.exportAutocrypt(selfFpr, self), true)});
        QByteArray raw;
        if (encrypt) {
            const QString peer = fprOf(to);
            mail::Keyring::ensure(ctx, peer);
            m.innerHeaders = inner;
            if (gossip)
                m.extraHeaders.append({"Autocrypt-Gossip",
                                       crypto::Autocrypt::format(toAddr, ctx.pgp.exportAutocrypt(peer, toAddr), false)});
            const QByteArray armored = ctx.pgp.encrypt(mime::Builder::contentEntity(m, true), {peer, selfFpr}, selfFpr);
            const QString hidden = QStringLiteral("...");
            raw = mime::Builder::headerBlock(m, &hidden) + mime::Builder::encryptedBody(armored);
        } else {
            m.extraHeaders.append(inner);
            raw = mime::Builder::build(m);
        }
        m_srv->deliver(toAddr, raw);
    }

    // Alice and Mallory have mailed each other, so each holds the other's key.
    void introduce(Account *a, Account *b)
    {
        Draft d;
        d.to = {{QString(), b->ctx().config.get(cfg::Addr)}};
        d.subject = QStringLiteral("hello");
        d.text = QStringLiteral("hello");
        mail::Compose::queue(a->ctx(), d);
        sync(a);
        sync(b);
        d.to = {{QString(), a->ctx().config.get(cfg::Addr)}};
        mail::Compose::queue(b->ctx(), d);
        sync(b);
        sync(a);
    }

private Q_SLOTS:
    void init()
    {
        qputenv("E3MAIL_NO_KEYCHAIN", "1");
        m_dir = std::make_unique<QTemporaryDir>();
        m_srv = std::make_unique<FakeMailServer>();
        m_mgr = std::make_unique<AccountManager>(m_dir->path() + QStringLiteral("/main"));
    }

    void cleanup()
    {
        m_mgr2.reset();
        m_mgr.reset();
        m_srv.reset();
        m_dir.reset();
    }

    void invitesRoundTrip()
    {
        SecureJoin::Invite inv{QStringLiteral("0123456789ABCDEF0123456789ABCDEF01234567"), QStringLiteral("ann+x@x.test"),
                               QStringLiteral("Ann Ödegaard & Co"), QStringLiteral("AbCdEfGhIjKlMn-_12"),
                               QStringLiteral("zyxwvutsrqponm_-98")};
        const QString text = SecureJoin::format(inv);
        QVERIFY(text.startsWith(QLatin1String("OPENPGP4FPR:0123456789ABCDEF")));
        QVERIFY(!text.contains(u' '));
        const auto back = SecureJoin::parse(text);
        QVERIFY(back);
        QCOMPARE(back->fpr, inv.fpr);
        QCOMPARE(back->addr, inv.addr);
        QCOMPARE(back->name, inv.name);
        QCOMPARE(back->invitenumber, inv.invitenumber);
        QCOMPARE(back->authcode, inv.authcode);

        // Delta Chat's own link form, as its apps share it.
        const auto dc = SecureJoin::parse(QStringLiteral(
            "https://i.delta.chat/#0123456789ABCDEF0123456789ABCDEF01234567&v=3&i=AbCdEfGhIjKlMn&s=zyxwvutsrqpon"
            "m&a=dc%40x.test&n=Dee+Cee"));
        QVERIFY(dc);
        QCOMPARE(dc->addr, QStringLiteral("dc@x.test"));
        QCOMPARE(dc->name, QStringLiteral("Dee Cee"));
        // Lower case and a percent-encoded '#' from a phone's camera app.
        QVERIFY(SecureJoin::parse(QStringLiteral("openpgp4fpr:0123456789abcdef0123456789abcdef01234567%23a=x%40x.test"
                                                 "&i=AbCdEfGhIjKlMn&s=zyxwvutsrqponm")));

        // Groups and channels are not something e3mail has; missing tokens,
        // a short fingerprint or no address are not invites.
        QVERIFY(!SecureJoin::parse(text + QStringLiteral("&g=Team&x=abcdefghijklmn")));
        QVERIFY(!SecureJoin::parse(QStringLiteral("OPENPGP4FPR:0123456789ABCDEF0123456789ABCDEF01234567#a=x%40x.test")));
        QVERIFY(!SecureJoin::parse(QStringLiteral("OPENPGP4FPR:0123#a=x%40x.test&i=AbCdEfGhIjKlMn&s=zyxwvutsrqponm")));
        QVERIFY(!SecureJoin::parse(
            QStringLiteral("OPENPGP4FPR:0123456789ABCDEF0123456789ABCDEF01234567#i=AbCdEfGhIjKlMn&s=zyxwvutsrqponm")));
        QVERIFY(!SecureJoin::parse(QStringLiteral("E3PAIR1:whatever")));
    }

    void bothVerified()
    {
        Account *a = makeAccount(alice());
        Account *b = makeAccount(bob());
        const QString text = SecureJoin::invite(a->ctx());
        QCOMPARE(SecureJoin::invite(a->ctx()), text); // the same until renewed
        const auto inv = SecureJoin::parse(text);
        QVERIFY(inv);
        QCOMPARE(inv->fpr, fprOf(a));

        SecureJoin::join(b->ctx(), *inv);
        QCOMPARE(joinState(b, alice()), SecureJoin::JoinState::Requested);
        // The first step is cleartext (Bob has no key of Alice's yet) and
        // carries only the invite number, never the token.
        sync(b);
        const QByteArray request = m_srv->envelopes().last().data;
        QVERIFY(request.contains("Secure-Join: vc-request"));
        QVERIFY(request.contains(inv->invitenumber.toUtf8()));
        QVERIFY(!request.contains(inv->authcode.toUtf8()));
        QVERIFY(request.contains("Autocrypt: addr=bob@x.test"));

        exchange(a, b);
        QVERIFY(verified(a, bob(), fprOf(b)));
        QVERIFY(verified(b, alice(), fprOf(a)));
        QCOMPARE(joinState(b, alice()), SecureJoin::JoinState::Done);
        // The token only ever travelled encrypted.
        for (const auto &e : m_srv->envelopes())
            QVERIFY2(!e.data.contains(inv->authcode.toUtf8()), e.data.constData());

        // Nothing of it is mail to read, on either side.
        QCOMPARE(a->ctx().db.queryInt("SELECT count(*) FROM messages WHERE hidden=0").value_or(-1), 0);
        QCOMPARE(b->ctx().db.queryInt("SELECT count(*) FROM messages WHERE hidden=0").value_or(-1), 0);
        // Each is now someone the other's mail is trusted from.
        QVERIFY(mail::Contacts::isTrusted(a->ctx(), bob()));
        QVERIFY(mail::Contacts::isTrusted(b->ctx(), alice()));
        QCOMPARE(a->ctx().db.queryInt("SELECT count(*) FROM ops WHERE kind='contact.verify'").value_or(0), 1);
        QVERIFY(a->ctx().db.queryInt("SELECT count(*) FROM ops WHERE kind='sj.invite'").value_or(0) >= 1);
        QVERIFY(b->ctx().db.queryInt("SELECT count(*) FROM ops WHERE kind='sj.join'").value_or(0) >= 2);

        // Mail between them is now marked verified.
        Draft d;
        d.to = {{QString(), alice()}};
        d.subject = QStringLiteral("after");
        d.text = QStringLiteral("after");
        mail::Compose::queue(b->ctx(), d);
        sync(b);
        sync(a);
        const auto inbox = mail::Search::list(a->ctx(), SearchQuery::forTag(tag::Inbox));
        QCOMPARE(inbox.size(), 1);
        QVERIFY(mail::Search::detail(a->ctx(), inbox[0].id)->verified);
    }

    void shortcutWhenKeyKnown()
    {
        Account *a = makeAccount(alice());
        Account *b = makeAccount(bob());
        introduce(a, b);
        const auto inv = SecureJoin::parse(SecureJoin::invite(a->ctx()));
        SecureJoin::join(b->ctx(), *inv);
        QCOMPARE(joinState(b, alice()), SecureJoin::JoinState::AuthSent);
        exchange(a, b);
        QVERIFY(verified(a, bob(), fprOf(b)));
        QVERIFY(verified(b, alice(), fprOf(a)));
        QCOMPARE(joinState(b, alice()), SecureJoin::JoinState::Done);
    }

    // The invite named a key that is not Alice's: Bob never sends the token.
    void wrongFingerprint()
    {
        Account *a = makeAccount(alice());
        Account *b = makeAccount(bob());
        Account *m = makeAccount(mallory());
        auto inv = *SecureJoin::parse(SecureJoin::invite(a->ctx()));
        inv.fpr = mail::Keyring::ensureSelfKey(m->ctx(), mallory());
        SecureJoin::join(b->ctx(), inv);
        exchange(a, b);
        QCOMPARE(joinState(b, alice()), SecureJoin::JoinState::Requested);
        QVERIFY(!mail::Contacts::get(b->ctx(), alice())->verified);
        QVERIFY(!mail::Contacts::get(a->ctx(), bob()) || !mail::Contacts::get(a->ctx(), bob())->verified);
        for (const auto &e : m_srv->envelopes())
            QVERIFY(!e.data.contains("vc-request-with-auth"));
    }

    void wrongToken()
    {
        Account *a = makeAccount(alice());
        Account *b = makeAccount(bob());
        auto inv = *SecureJoin::parse(SecureJoin::invite(a->ctx()));
        inv.authcode = QStringLiteral("notTheRealToken123");
        SecureJoin::join(b->ctx(), inv);
        exchange(a, b);
        // Bob verified Alice (her key was the one named), Alice did not
        // verify Bob, and never confirmed.
        QVERIFY(verified(b, alice(), fprOf(a)));
        QVERIFY(!mail::Contacts::get(a->ctx(), bob())->verified);
        QCOMPARE(joinState(b, alice()), SecureJoin::JoinState::AuthSent);
    }

    void renewedInviteStopsWorking()
    {
        Account *a = makeAccount(alice());
        Account *b = makeAccount(bob());
        const auto old = *SecureJoin::parse(SecureJoin::invite(a->ctx()));
        const auto fresh = *SecureJoin::parse(SecureJoin::renewInvite(a->ctx()));
        QVERIFY(fresh.invitenumber != old.invitenumber);
        SecureJoin::join(b->ctx(), old);
        exchange(a, b);
        QCOMPARE(joinState(b, alice()), SecureJoin::JoinState::Requested);
        for (const auto &e : m_srv->envelopes())
            QVERIFY(!e.data.contains("vc-auth-required") && !e.data.contains("BEGIN PGP MESSAGE"));
    }

    // Mallory saw Alice's code and claims Bob's key. Her token is right; her
    // signature is not Bob's.
    void tokenSignedByAnotherKey()
    {
        Account *a = makeAccount(alice());
        Account *b = makeAccount(bob());
        Account *m = makeAccount(mallory());
        introduce(a, m);
        mail::Keyring::ensureSelfKey(b->ctx(), bob());
        const auto inv = *SecureJoin::parse(SecureJoin::invite(a->ctx()));
        forge(m, a, {{"Secure-Join", "vc-request-with-auth"}, {"Secure-Join-Auth", inv.authcode.toUtf8()},
                     {"Secure-Join-Fingerprint", fprOf(b).toUtf8()}});
        sync(a);
        QVERIFY(!mail::Contacts::get(a->ctx(), mallory())->verified);
        QCOMPARE(a->ctx().db.queryInt("SELECT count(*) FROM contacts WHERE verified=1").value_or(-1), 0);

        // The same in cleartext, naming her own key: refused too.
        forge(m, a,
              {{"Secure-Join", "vc-request-with-auth"}, {"Secure-Join-Auth", inv.authcode.toUtf8()},
               {"Secure-Join-Fingerprint", fprOf(m).toUtf8()}},
              false);
        sync(a);
        QVERIFY(!mail::Contacts::get(a->ctx(), mallory())->verified);

        // Signed by the key she names, but not saying it was meant for Alice
        // (no gossip of Alice's own key): it could be forwarded. Refused.
        forge(m, a,
              {{"Secure-Join", "vc-request-with-auth"}, {"Secure-Join-Auth", inv.authcode.toUtf8()},
               {"Secure-Join-Fingerprint", fprOf(m).toUtf8()}},
              true, false);
        sync(a);
        QVERIFY(!mail::Contacts::get(a->ctx(), mallory())->verified);

        // Properly encrypted and signed by the key she names, the token is
        // what the invite promises: Mallory, as Mallory, is verified.
        forge(m, a, {{"Secure-Join", "vc-request-with-auth"}, {"Secure-Join-Auth", inv.authcode.toUtf8()},
                     {"Secure-Join-Fingerprint", fprOf(m).toUtf8()}});
        sync(a);
        QVERIFY(verified(a, mallory(), fprOf(m)));
        QVERIFY(!mail::Contacts::get(a->ctx(), bob()));
    }

    // Alice's second device fetches Bob's steps and answers them.
    void anotherDeviceAnswers()
    {
        Account *a = makeAccount(alice());
        Account *b = makeAccount(bob());
        const auto inv = *SecureJoin::parse(SecureJoin::invite(a->ctx()));
        const QString root = m_dir->path() + QStringLiteral("/alice2");
        QVERIFY(cloneDevice(a, root));
        m_mgr2 = std::make_unique<AccountManager>(root);
        Account *a2 = m_mgr2->account(a->id());
        QVERIFY(a2);
        SecureJoin::join(b->ctx(), inv);
        exchange(a2, b);
        QVERIFY(verified(a2, bob(), fprOf(b)));
        QVERIFY(verified(b, alice(), fprOf(a)));
        QCOMPARE(joinState(b, alice()), SecureJoin::JoinState::Done);
    }

    void ownCodeAndExpiry()
    {
        Account *a = makeAccount(alice());
        Account *b = makeAccount(bob());
        const auto own = *SecureJoin::parse(SecureJoin::invite(a->ctx()));
        QVERIFY_THROWS_EXCEPTION(mail::ComposeError, SecureJoin::join(a->ctx(), own));

        SecureJoin::join(b->ctx(), own);
        const qint64 later = QDateTime::currentMSecsSinceEpoch() + SecureJoin::kJoinTimeoutMs + 1000;
        QCOMPARE(SecureJoin::expire(b->ctx(), later), 1);
        QCOMPARE(joinState(b, alice()), SecureJoin::JoinState::Failed);
        QCOMPARE(SecureJoin::expire(b->ctx(), later), 0);
    }
};

QTEST_GUILESS_MAIN(TestSecureJoin)
#include "tst_securejoin.moc"
