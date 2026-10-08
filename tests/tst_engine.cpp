// SPDX-License-Identifier: MPL-2.0
#include "FakeMailServer.h"
#include "engine/AccountManager.h"
#include "engine/MailWorker.h"
#include "mail/Compose.h"
#include "mail/Contacts.h"
#include "mail/Organize.h"
#include "mail/Policy.h"
#include "mail/Search.h"
#include "mail/VCard.h"
#include "store/OpLog.h"
#include "store/Config.h"
#include "store/Database.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace e3;

// Drives two (or three) real accounts through the real engine against the
// in-process mail system: setup, gating, the Autocrypt bootstrap, an
// encrypted reply, threading, server retention, Bcc.
class TestEngine : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<FakeMailServer> m_srv;
    std::unique_ptr<AccountManager> m_mgr;

    Account *makeAccount(const QString &addr, const QString &protocol)
    {
        m_srv->addUser(addr, QStringLiteral("pw"));
        Account *a = m_mgr->create();
        Account::Settings s;
        s.addr = addr;
        s.displayName = addr.section(u'@', 0, 0);
        s.protocol = protocol;
        s.inHost = s.smtpHost = QStringLiteral("127.0.0.1");
        s.inPort = protocol == QLatin1String("pop3") ? m_srv->pop3Port() : m_srv->imapPort();
        s.inSecurity = s.smtpSecurity = QStringLiteral("plain");
        s.smtpPort = m_srv->smtpPort();
        s.inPassword = QStringLiteral("pw");
        a->configure(s);
        return a;
    }

    static void sync(Account *a) { MailWorker::runOnce(a->dir(), a->credentials()); }

    static Draft draft(const QString &to, const QString &subject, const QString &text)
    {
        Draft d;
        d.to = {{QString(), to}};
        d.subject = subject;
        d.text = text;
        return d;
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

    void conversation()
    {
        Account *alice = makeAccount(QStringLiteral("alice@x.test"), QStringLiteral("imap"));
        Account *bob = makeAccount(QStringLiteral("bob@x.test"), QStringLiteral("pop3"));
        makeAccount(QStringLiteral("carol@x.test"), QStringLiteral("imap"));

        // Something already on Bob's server before e3mail first looks is never
        // deleted: retention is not retroactive.
        m_srv->deliver(QStringLiteral("bob@x.test"),
                       "From: old@x.test\r\nSubject: old\r\nMessage-ID: <old@x.test>\r\n\r\nold\r\n");
        sync(alice);
        sync(bob);
        QCOMPARE(m_srv->mailbox(QStringLiteral("bob@x.test")).size(), 1);

        // 1. Alice writes first. She has no key for Bob, so it goes cleartext,
        //    carrying her Autocrypt header.
        Draft first = draft(QStringLiteral("bob@x.test"), QStringLiteral("Thursday's numbers"),
                            QStringLiteral("Hi Bob,\nnumbers attached."));
        first.cc = {{QStringLiteral("Carol"), QStringLiteral("carol@x.test")}};
        first.attachments = {{QStringLiteral("q3.csv"), QStringLiteral("text/csv"), "a,b\n1,2\n"}};
        first.importance = 1;
        const auto readiness = mail::Policy::evaluate(alice->ctx(), {QStringLiteral("bob@x.test")}, SendEncryption::Auto);
        QVERIFY(!readiness.willEncrypt);
        QCOMPARE(readiness.missingKeys, QStringList{QStringLiteral("bob@x.test")});
        const qint64 sentId = mail::Compose::queue(alice->ctx(), first);
        sync(alice);
        QCOMPARE(mail::Search::detail(alice->ctx(), sentId)->state, MessageState::Sent);
        QCOMPARE(mail::Search::count(alice->ctx(), tag::Sent, false), 1);
        QVERIFY(m_srv->envelopes().last().data.contains("Autocrypt: addr=alice@x.test"));

        // 2. Bob is a stranger to Alice's address, so it waits in Unverified,
        //    readable, with Alice's key learned but not verified.
        sync(bob);
        // (the old message is from a stranger too)
        QCOMPARE(mail::Search::count(bob->ctx(), tag::Unverified, false), 2);
        QCOMPARE(mail::Search::count(bob->ctx(), tag::Inbox, false), 0);
        const auto held = mail::Search::list(bob->ctx(), SearchQuery::forTag(tag::Unverified));
        qint64 heldId = 0;
        for (const auto &h : held) {
            if (h.from.addr == QLatin1String("alice@x.test"))
                heldId = h.id;
        }
        const auto msg = *mail::Search::detail(bob->ctx(), heldId);
        QCOMPARE(msg.subject, QStringLiteral("Thursday's numbers"));
        QCOMPARE(msg.cc.value(0).addr, QStringLiteral("carol@x.test"));
        QCOMPARE(msg.importance, 1);
        QCOMPARE(msg.attachments.size(), 1);
        QCOMPARE(msg.attachments[0].filename, QStringLiteral("q3.csv"));
        QVERIFY(!msg.encrypted);
        QVERIFY(msg.held);
        const auto aliceContact = mail::Contacts::get(bob->ctx(), QStringLiteral("alice@x.test"));
        QVERIFY(!aliceContact->fingerprint.isEmpty());
        QVERIFY(!aliceContact->verified);
        // Delete-after-download removed it from the server; the old one stays.
        QCOMPARE(m_srv->mailbox(QStringLiteral("bob@x.test")).size(), 1);

        // 3. Accepting Alice releases her mail into the Inbox.
        QCOMPARE(mail::Organize::accept(bob->ctx(), QStringLiteral("alice@x.test")), 1);
        QCOMPARE(mail::Search::count(bob->ctx(), tag::Unverified, false), 1);
        QCOMPARE(mail::Search::count(bob->ctx(), tag::Inbox, false), 1);

        // 4. Bob replies. Reply addresses Alice, not Bob's own address, and is
        //    encrypted because Alice's key is now known.
        Draft reply = mail::Compose::reply(bob->ctx(), msg.id, false);
        QCOMPARE(reply.to.value(0).addr, QStringLiteral("alice@x.test"));
        QCOMPARE(reply.subject, QStringLiteral("Re: Thursday's numbers"));
        reply.text = QStringLiteral("The second column is off by one.") + reply.text;
        const qint64 replyId = mail::Compose::queue(bob->ctx(), reply);
        QVERIFY(mail::Search::detail(bob->ctx(), replyId)->encrypted);
        sync(bob);
        const QByteArray wire = m_srv->envelopes().last().data;
        QVERIFY(wire.contains("multipart/encrypted"));
        QVERIFY(!wire.contains("off by one"));
        QVERIFY(!wire.contains("Thursday"));      // protected subject
        QVERIFY(wire.contains("Subject: ..."));

        // 5. Alice wrote to Bob, so he is known: straight to her Inbox,
        //    decrypted, signed, threaded onto her original.
        sync(alice);
        const auto inbox = mail::Search::list(alice->ctx(), SearchQuery::forTag(tag::Inbox));
        QCOMPARE(inbox.size(), 1);
        const auto got = *mail::Search::detail(alice->ctx(), inbox.first().id);
        QCOMPARE(got.subject, QStringLiteral("Re: Thursday's numbers"));
        QVERIFY(got.bodyText.startsWith(QStringLiteral("The second column is off by one.")));
        QVERIFY(got.encrypted);
        QVERIFY(got.signedBySender);
        QVERIFY(!got.verified);
        QCOMPARE(got.threadId, mail::Search::detail(alice->ctx(), sentId)->threadId);
        QCOMPARE(mail::Search::thread(alice->ctx(), got.id), (QList<qint64>{sentId, got.id}));

        // 6. Full-text search finds the decrypted body.
        SearchQuery q;
        q.tag = tag::All;
        q.text = QStringLiteral("column");
        QCOMPARE(mail::Search::list(alice->ctx(), q).size(), 1);

        // 7. Bcc reaches the envelope, never a header.
        Draft secret = draft(QStringLiteral("bob@x.test"), QStringLiteral("bcc test"), QStringLiteral("x"));
        secret.bcc = {{QString(), QStringLiteral("carol@x.test")}};
        mail::Compose::queue(alice->ctx(), secret);
        sync(alice);
        const auto env = m_srv->envelopes().last();
        QVERIFY(env.rcpts.contains(QStringLiteral("carol@x.test")));
        QVERIFY(!env.data.contains("carol@x.test"));
    }

    void strictRefusesWithoutKey()
    {
        Account *alice = makeAccount(QStringLiteral("alice@x.test"), QStringLiteral("imap"));
        alice->ctx().config.setInt(cfg::EncryptionMode, int(EncryptionMode::Strict));
        QVERIFY_THROWS_EXCEPTION(mail::ComposeError,
                                 mail::Compose::queue(alice->ctx(), draft(QStringLiteral("x@y.test"), QStringLiteral("s"),
                                                                          QStringLiteral("t"))));
        // And the padlock cannot ask for cleartext under strict.
        const auto r = mail::Policy::evaluate(alice->ctx(), {QStringLiteral("x@y.test")}, SendEncryption::Plaintext);
        QVERIFY(!r.canSend);
        QVERIFY(r.padlockLocked);
    }

    static QByteArray numbered(int i)
    {
        return QStringLiteral("From: a@b.test\r\nSubject: m%1\r\nMessage-ID: <m%1@b.test>\r\n"
                              "Date: Mon, 1 Jan 2024 10:%2:00 +0000\r\n\r\nbody %1\r\n")
            .arg(i)
            .arg(i % 60, 2, 10, QLatin1Char('0'))
            .toUtf8();
    }

    static bool has(Account *a, int i)
    {
        return a->ctx().db.queryInt("SELECT 1 FROM messages WHERE message_id=?", QStringLiteral("m%1@b.test").arg(i))
            .has_value();
    }

    void firstSyncPagesNewestFirst()
    {
        m_srv->addUser(QStringLiteral("big@x.test"), QStringLiteral("pw"));
        for (int i = 0; i < 250; ++i)
            m_srv->deliver(QStringLiteral("big@x.test"), numbered(i));
        Account *a = m_mgr->create();
        Account::Settings s;
        s.addr = QStringLiteral("big@x.test");
        s.protocol = QStringLiteral("imap");
        s.inHost = s.smtpHost = QStringLiteral("127.0.0.1");
        s.inPort = m_srv->imapPort();
        s.smtpPort = m_srv->smtpPort();
        s.inSecurity = s.smtpSecurity = QStringLiteral("plain");
        s.inPassword = QStringLiteral("pw");
        a->configure(s);
        const auto count = [a] { return *a->ctx().db.queryInt("SELECT count(*) FROM messages"); };

        // A round is new mail, then the newest page of what was there.
        QCOMPARE(MailWorker::runOnce(a->dir(), a->credentials()), 150);
        QCOMPARE(count(), 100);
        QVERIFY(has(a, 249) && has(a, 150) && !has(a, 149));

        // New mail goes first, even mid-backfill, and is not "found there".
        // Retention keeps it on the server for now.
        a->ctx().config.set(cfg::ServerRetention, QStringLiteral("keep"));
        a->ctx().config.setInt(cfg::ServerKeepDays, 30);
        m_srv->deliver(QStringLiteral("big@x.test"), numbered(1000));
        QCOMPARE(MailWorker::runOnce(a->dir(), a->credentials()), 50);
        QVERIFY(has(a, 1000) && has(a, 50) && !has(a, 49));

        // A fresh connection resumes where the last stopped: nothing twice.
        QCOMPARE(MailWorker::runOnce(a->dir(), a->credentials()), 0);
        QCOMPARE(count(), 251);
        QCOMPARE(m_srv->fetchCount, 251);

        QCOMPARE(m_srv->mailbox(QStringLiteral("big@x.test")).size(), 251);

        // A new UIDVALIDITY re-reads everything and adds nothing. Each copy
        // keeps what it was: the 250 were there before e3mail looked, and
        // the new one is still retention's to delete.
        m_srv->uidValidity = 778;
        QCOMPARE(MailWorker::runOnce(a->dir(), a->credentials()), 151);
        QCOMPARE(MailWorker::runOnce(a->dir(), a->credentials()), 51);
        a->ctx().config.setInt(cfg::ServerKeepDays, 0);
        QCOMPARE(MailWorker::runOnce(a->dir(), a->credentials()), 0);
        QCOMPARE(count(), 251);
        QCOMPARE(m_srv->mailbox(QStringLiteral("big@x.test")).size(), 250);
        QVERIFY(!m_srv->mailbox(QStringLiteral("big@x.test")).join().contains("m1000"));
    }

    void firstSyncIsNotNewMail()
    {
        m_srv->addUser(QStringLiteral("big@x.test"), QStringLiteral("pw"));
        for (int i = 0; i < 150; ++i)
            m_srv->deliver(QStringLiteral("big@x.test"), numbered(i));
        Account *a = m_mgr->create();
        Account::Settings s;
        s.addr = QStringLiteral("big@x.test");
        s.protocol = QStringLiteral("imap");
        s.inHost = s.smtpHost = QStringLiteral("127.0.0.1");
        s.inPort = m_srv->imapPort();
        s.smtpPort = m_srv->smtpPort();
        s.inSecurity = s.smtpSecurity = QStringLiteral("plain");
        s.inPassword = QStringLiteral("pw");
        a->configure(s);
        mail::Contacts::touch(a->ctx(), QStringLiteral("a@b.test"), QString(), ContactOrigin::Manual);
        QSignalSpy spy(a, &Account::newMail);
        a->start();
        // The worker goes straight on from page to page.
        QTRY_COMPARE_WITH_TIMEOUT(*a->ctx().db.queryInt("SELECT count(*) FROM messages"), 150, 10000);
        QTRY_COMPARE(a->olderRemaining(), 0);
        QCOMPARE(spy.size(), 0);
        m_srv->deliver(QStringLiteral("big@x.test"), numbered(500));
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 10000);
        a->stop();
    }

    void addressBook()
    {
        Account *bob = makeAccount(QStringLiteral("bob@x.test"), QStringLiteral("imap"));
        auto &ctx = bob->ctx();
        const auto ops = [&ctx](const QString &kind) {
            return *ctx.db.queryInt("SELECT count(*) FROM ops WHERE kind=?", kind);
        };

        // Added by hand: in the book, so their mail is trusted.
        QVERIFY(!mail::Contacts::create(ctx, QStringLiteral("not an address"), QString()));
        const auto id = mail::Contacts::create(ctx, QStringLiteral("Ada@B.test"), QStringLiteral("Ada"));
        QVERIFY(id);
        QVERIFY(!mail::Contacts::create(ctx, QStringLiteral("ada@b.test"), QString())); // already there
        QVERIFY(mail::Contacts::isTrusted(ctx, QStringLiteral("ada@b.test")));
        QCOMPARE(ops(op::ContactEdit), 1);

        ContactInfo d = *mail::Contacts::byId(ctx, *id);
        d.organization = QStringLiteral("  Engines  ");
        d.birthday = QStringLiteral("not a date");
        d.phones = {{QStringLiteral("work"), QStringLiteral(" +1 ")}, {QStringLiteral("home"), QStringLiteral("  ")}};
        mail::Contacts::setDetails(ctx, *id, d);
        auto c = *mail::Contacts::byId(ctx, *id);
        QCOMPARE(c.organization, QStringLiteral("Engines"));
        QVERIFY(c.birthday.isEmpty());
        QCOMPARE(c.phones, (QList<ContactPhone>{{QStringLiteral("work"), QStringLiteral("+1")}}));
        QCOMPARE(ops(op::ContactDetails), 1);
        mail::Contacts::setDetails(ctx, *id, c); // nothing changed, nothing recorded
        QCOMPARE(ops(op::ContactDetails), 1);

        // Removed: their next mail waits in Unverified again.
        mail::Contacts::remove(ctx, *id);
        QVERIFY(!mail::Contacts::get(ctx, QStringLiteral("ada@b.test")));
        QCOMPARE(ops(op::ContactRemove), 1);
        QCOMPARE(*ctx.db.queryInt("SELECT count(*) FROM contact_phones"), 0);
        m_srv->deliver(QStringLiteral("bob@x.test"), "From: ada@b.test\r\nSubject: hi\r\n\r\nx\r\n");
        sync(bob);
        QCOMPARE(mail::Search::count(ctx, tag::Unverified, false), 1);

        // Import fills only what is missing, and takes people into the book.
        mail::Contacts::create(ctx, QStringLiteral("grace@c.test"), QStringLiteral("Grace H."));
        const auto result = mail::Contacts::import(
            ctx, mail::VCard::parse("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Grace Hopper\r\nEMAIL:grace@c.test\r\n"
                                    "ORG:Navy\r\nEND:VCARD\r\n"
                                    "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Ada Lovelace\r\nEMAIL:ada@b.test\r\n"
                                    "EMAIL:ada2@b.test\r\nTEL;TYPE=CELL:+44\r\nEND:VCARD\r\n"
                                    "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Nobody\r\nEND:VCARD\r\n"));
        QCOMPARE(result.added, 1);    // ada2
        QCOMPARE(result.updated, 2);  // grace (organization), ada (seen, now in the book)
        QCOMPARE(result.skipped, 1);
        c = *mail::Contacts::get(ctx, QStringLiteral("grace@c.test"));
        QCOMPARE(c.name, QStringLiteral("Grace H.")); // kept
        QCOMPARE(c.organization, QStringLiteral("Navy"));
        QVERIFY(mail::Contacts::isTrusted(ctx, QStringLiteral("ada@b.test")));
        QCOMPARE(mail::Contacts::get(ctx, QStringLiteral("ada2@b.test"))->phones.value(0).number, QStringLiteral("+44"));
    }

    void repliesQuoteHtmlAsHtml()
    {
        Account *bob = makeAccount(QStringLiteral("bob@x.test"), QStringLiteral("imap"));
        m_srv->deliver(QStringLiteral("bob@x.test"),
                       "From: Ada <ada@b.test>\r\nSubject: plans\r\nMessage-ID: <h1@b.test>\r\n"
                       "MIME-Version: 1.0\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
                       "<p>The <b>plan</b> is <a href=\"https://plan.test/\">here</a>.</p>"
                       "<script>x()</script>\r\n");
        sync(bob);
        const qint64 id = *bob->ctx().db.queryInt("SELECT id FROM messages WHERE message_id='h1@b.test'");

        const Draft reply = mail::Compose::reply(bob->ctx(), id, false);
        QVERIFY2(reply.html.contains(QLatin1String("<blockquote><p>The <b>plan</b> is <a href=\"https://plan.test/\">")),
                 qPrintable(reply.html));
        QVERIFY(reply.html.contains(QLatin1String("Ada wrote:")));
        QVERIFY(!reply.html.contains(QLatin1String("script")));
        QVERIFY(reply.text.contains(QLatin1String("> The plan is"))); // the plain alternative stays

        const Draft fwd = mail::Compose::forward(bob->ctx(), id);
        QVERIFY(fwd.html.contains(QLatin1String("Forwarded message")));
        QVERIFY(fwd.html.contains(QLatin1String("<b>plan</b>")));

        // Plain mail is quoted as plain text, as before.
        m_srv->deliver(QStringLiteral("bob@x.test"), "From: ada@b.test\r\nSubject: p\r\nMessage-ID: <p1@b.test>\r\n\r\nplain\r\n");
        sync(bob);
        const qint64 plain = *bob->ctx().db.queryInt("SELECT id FROM messages WHERE message_id='p1@b.test'");
        QVERIFY(mail::Compose::reply(bob->ctx(), plain, false).html.isEmpty());
    }

    void selfIsEncrypted()
    {
        Account *alice = makeAccount(QStringLiteral("alice@x.test"), QStringLiteral("imap"));
        Account *bob = makeAccount(QStringLiteral("bob@x.test"), QStringLiteral("imap"));
        const QString self = QStringLiteral("Alice@X.test"); // compared normalised

        // Our own key is always there, whatever the mode.
        for (const auto mode : {EncryptionMode::Lenient, EncryptionMode::Opportunistic, EncryptionMode::Strict}) {
            alice->ctx().config.setInt(cfg::EncryptionMode, int(mode));
            const auto r = mail::Policy::evaluate(alice->ctx(), {self}, SendEncryption::Auto);
            QVERIFY(r.willEncrypt);
            QVERIFY(r.canSend);
            QVERIFY(r.missingKeys.isEmpty());
        }
        alice->ctx().config.setInt(cfg::EncryptionMode, int(EncryptionMode::Opportunistic));

        // A note to self goes out encrypted.
        const qint64 noteId = mail::Compose::queue(alice->ctx(), draft(self, QStringLiteral("note"),
                                                                       QStringLiteral("remember the milk")));
        QVERIFY(mail::Search::detail(alice->ctx(), noteId)->encrypted);
        sync(alice);
        QByteArray wire = m_srv->envelopes().last().data;
        QVERIFY(wire.contains("multipart/encrypted"));
        QVERIFY(!wire.contains("milk"));
        // Its copy coming back is the same message, not a second one; it
        // shows in Sent and, being addressed to us, in the Inbox.
        sync(alice);
        QCOMPARE(mail::Search::count(alice->ctx(), tag::Sent, false), 1);
        QCOMPARE(mail::Search::count(alice->ctx(), tag::Inbox, false), 1);
        QCOMPARE(alice->ctx().db.queryInt("SELECT count(*) FROM messages").value_or(0), 1);

        // With a stranger along, the stranger decides, and only the stranger
        // is missing a key.
        const auto mixed = mail::Policy::evaluate(alice->ctx(), {self, QStringLiteral("x@y.test")},
                                                  SendEncryption::Auto);
        QVERIFY(!mixed.willEncrypt);
        QCOMPARE(mixed.missingKeys, QStringList{QStringLiteral("x@y.test")});

        // Bob learns Alice's key; a Bcc to himself keeps his reply encrypted.
        mail::Compose::queue(alice->ctx(), draft(QStringLiteral("bob@x.test"), QStringLiteral("hi"), QStringLiteral("x")));
        sync(alice);
        sync(bob);
        Draft withBcc = draft(QStringLiteral("alice@x.test"), QStringLiteral("re"), QStringLiteral("secret plans"));
        withBcc.bcc = {{QString(), QStringLiteral("bob@x.test")}};
        QVERIFY(mail::Search::detail(bob->ctx(), mail::Compose::queue(bob->ctx(), withBcc))->encrypted);
        sync(bob);
        wire = m_srv->envelopes().last().data;
        QVERIFY(wire.contains("multipart/encrypted"));
        QVERIFY(!wire.contains("secret plans"));
    }

    void sentToSelfInInbox()
    {
        Account *alice = makeAccount(QStringLiteral("alice@x.test"), QStringLiteral("imap"));
        makeAccount(QStringLiteral("bob@x.test"), QStringLiteral("imap"));
        sync(alice);
        auto ids = [&](const QString &t) {
            QList<qint64> out;
            for (const auto &s : mail::Search::list(alice->ctx(), SearchQuery::forTag(t)))
                out.append(s.id);
            return out;
        };

        // To self, Cc to self, Bcc to self: each is in Sent and in the Inbox.
        const qint64 to = mail::Compose::queue(alice->ctx(), draft(QStringLiteral("ALICE@x.test"), QStringLiteral("to"),
                                                                   QStringLiteral("a")));
        Draft cc = draft(QStringLiteral("bob@x.test"), QStringLiteral("cc"), QStringLiteral("b"));
        cc.cc = {{QString(), QStringLiteral("alice@x.test")}};
        const qint64 ccId = mail::Compose::queue(alice->ctx(), cc);
        Draft bcc = draft(QStringLiteral("bob@x.test"), QStringLiteral("bcc"), QStringLiteral("c"));
        bcc.bcc = {{QString(), QStringLiteral("alice@x.test")}};
        const qint64 bccId = mail::Compose::queue(alice->ctx(), bcc);
        // Mail to someone else stays out of the Inbox.
        const qint64 other = mail::Compose::queue(alice->ctx(), draft(QStringLiteral("bob@x.test"), QStringLiteral("o"),
                                                                      QStringLiteral("d")));
        // Not yet sent: only in Sent (as pending).
        QVERIFY(!ids(tag::Inbox).contains(to));
        sync(alice);
        sync(alice);

        const auto inbox = ids(tag::Inbox);
        QVERIFY(inbox.contains(to));
        QVERIFY(inbox.contains(ccId));
        QVERIFY(inbox.contains(bccId));
        QVERIFY(!inbox.contains(other));
        QCOMPARE(ids(tag::Sent).size(), 4);
        // Written by us, so it arrives read.
        QCOMPARE(mail::Search::count(alice->ctx(), tag::Inbox, true), 0);

        // Archiving takes it out of the Inbox only; All mail holds everything.
        mail::Organize::archive(alice->ctx(), to, true);
        QVERIFY(!ids(tag::Inbox).contains(to));
        QVERIFY(ids(tag::Archive).contains(to));
        QVERIFY(ids(tag::Sent).contains(to));
        const auto all = ids(tag::All);
        for (const qint64 id : {to, ccId, bccId, other})
            QVERIFY(all.contains(id));

        // Trash takes it out of everything but Trash.
        mail::Organize::trash(alice->ctx(), ccId, TrashReason::User);
        QVERIFY(!ids(tag::Inbox).contains(ccId));
        QVERIFY(!ids(tag::Sent).contains(ccId));
        QVERIFY(!ids(tag::All).contains(ccId));
    }

    void blocklistTrashesOnArrival()
    {
        Account *bob = makeAccount(QStringLiteral("bob@x.test"), QStringLiteral("imap"));
        sync(bob);
        mail::Contacts::block(bob->ctx(), QStringLiteral("@spam.test"));
        m_srv->deliver(QStringLiteral("bob@x.test"), "From: a@spam.test\r\nSubject: buy\r\n\r\nnow\r\n");
        m_srv->deliver(QStringLiteral("bob@x.test"), "From: a@mail.spam.test\r\nSubject: sub\r\n\r\nx\r\n");
        sync(bob);
        QCOMPARE(mail::Search::count(bob->ctx(), tag::Trash, false), 1);
        // a subdomain is not the domain
        QCOMPARE(mail::Search::count(bob->ctx(), tag::Unverified, false), 1);
        const auto trashed = mail::Search::list(bob->ctx(), SearchQuery::forTag(tag::Trash));
        QCOMPARE(mail::Search::detail(bob->ctx(), trashed.first().id)->trashReason, TrashReason::Blocked);
    }

    void deadlines()
    {
        Account *bob = makeAccount(QStringLiteral("bob@x.test"), QStringLiteral("imap"));
        sync(bob);
        m_srv->deliver(QStringLiteral("bob@x.test"), "From: stranger@z.test\r\nSubject: hi\r\n\r\nx\r\n");
        sync(bob);
        auto &ctx = bob->ctx();
        const qint64 id = mail::Search::list(ctx, SearchQuery::forTag(tag::Unverified)).first().id;

        // 0 days in Unverified means never sweep.
        ctx.config.setInt(cfg::UnverifiedTrashDays, 0);
        ctx.db.run("UPDATE held SET held_at = 0");
        mail::Organize::housekeeping(ctx);
        QCOMPARE(mail::Search::count(ctx, tag::Unverified, false), 1);

        // Past the window it goes to Trash, recoverably.
        ctx.config.setInt(cfg::UnverifiedTrashDays, 30);
        mail::Organize::housekeeping(ctx);
        QCOMPARE(mail::Search::count(ctx, tag::Trash, false), 1);
        QCOMPARE(mail::Search::detail(ctx, id)->trashReason, TrashReason::Unaccepted);

        // Restoring unaccepted mail accepts its sender.
        mail::Organize::restore(ctx, id);
        QCOMPARE(mail::Search::count(ctx, tag::Inbox, false), 1);
        QVERIFY(mail::Contacts::isTrusted(ctx, QStringLiteral("stranger@z.test")));

        // Trash with a 0 day window destroys at the next housekeeping.
        ctx.config.setInt(cfg::TrashPurgeDays, 0);
        mail::Organize::trash(ctx, id, TrashReason::User);
        mail::Organize::housekeeping(ctx);
        QVERIFY(!mail::Search::detail(ctx, id).has_value());
    }

    void labelsAndArchive()
    {
        Account *bob = makeAccount(QStringLiteral("bob@x.test"), QStringLiteral("imap"));
        sync(bob);
        mail::Organize::accept(bob->ctx(), QStringLiteral("friend@z.test"));
        m_srv->deliver(QStringLiteral("bob@x.test"), "From: friend@z.test\r\nSubject: hi\r\n\r\nx\r\n");
        sync(bob);
        auto &ctx = bob->ctx();
        const qint64 id = mail::Search::list(ctx, SearchQuery::forTag(tag::Inbox)).first().id;
        const qint64 label = mail::Organize::createLabel(ctx, QStringLiteral("Reading list"), QStringLiteral("#2b8a3e"));
        QCOMPARE(mail::Organize::createLabel(ctx, QStringLiteral("reading LIST"), QString()), label);
        mail::Organize::setLabel(ctx, id, label, true);
        mail::Organize::archive(ctx, id, true);
        QCOMPARE(mail::Search::count(ctx, tag::Inbox, false), 0);
        QCOMPARE(mail::Search::count(ctx, tag::Archive, false), 1);
        SearchQuery byLabel;
        byLabel.labelId = label;
        QCOMPARE(mail::Search::list(ctx, byLabel).size(), 1);
        QCOMPARE(mail::Search::list(ctx, byLabel).first().labels.value(0).name, QStringLiteral("Reading list"));
        QVERIFY(ctx.db.queryInt("SELECT count(*) FROM ops WHERE kind='label.add'").value_or(0) >= 2);
    }
};

QTEST_GUILESS_MAIN(TestEngine)
#include "tst_engine.moc"
