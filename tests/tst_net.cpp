// SPDX-License-Identifier: MPL-2.0
#include "FakeMailServer.h"
#include "net/Imap.h"
#include "net/Pop3.h"
#include "net/Smtp.h"

#include <QtTest>

using namespace e3::net;

class TestNet : public QObject
{
    Q_OBJECT

    ServerSettings settings(quint16 port, const QString &user, const QString &pass = QStringLiteral("pw"))
    {
        ServerSettings s;
        s.host = QStringLiteral("127.0.0.1");
        s.port = port;
        s.security = Security::Plain;
        s.user = user;
        s.password = pass;
        return s;
    }

private Q_SLOTS:
    void smtpSendsAndDotStuffs()
    {
        FakeMailServer srv;
        srv.addUser(QStringLiteral("alice@x.test"), QStringLiteral("pw"));
        srv.addUser(QStringLiteral("bob@x.test"), QStringLiteral("pw"));
        SmtpClient smtp;
        smtp.connect(settings(srv.smtpPort(), QStringLiteral("alice@x.test")));
        const QByteArray msg = "Subject: hi\r\n\r\n.leading dot\r\nline\r\n";
        const QStringList refused = smtp.send(QStringLiteral("alice@x.test"),
                                              {QStringLiteral("bob@x.test"), QStringLiteral("reject@x.test")}, msg);
        smtp.quit();
        QCOMPARE(refused, QStringList{QStringLiteral("reject@x.test")});
        QCOMPARE(srv.mailbox(QStringLiteral("bob@x.test")).value(0), msg);
    }

    void smtpBadPassword()
    {
        FakeMailServer srv;
        srv.addUser(QStringLiteral("alice@x.test"), QStringLiteral("pw"));
        SmtpClient smtp;
        QVERIFY_THROWS_EXCEPTION(AuthError, smtp.connect(settings(srv.smtpPort(), QStringLiteral("alice@x.test"), QStringLiteral("nope"))));
    }

    void imapFetchAndDelete_data()
    {
        QTest::addColumn<bool>("authPlain");
        QTest::addColumn<bool>("uidPlus");
        QTest::newRow("sasl+uidplus") << true << true;
        QTest::newRow("login+expunge") << false << false;
    }

    void imapFetchAndDelete()
    {
        QFETCH(bool, authPlain);
        QFETCH(bool, uidPlus);
        FakeMailServer srv;
        srv.imapAuthPlain = authPlain;
        srv.imapUidPlus = uidPlus;
        srv.addUser(QStringLiteral("bob@x.test"), QStringLiteral("p\"w"));
        srv.deliver(QStringLiteral("bob@x.test"), "Subject: one\r\n\r\nbody one\r\n");
        srv.deliver(QStringLiteral("bob@x.test"), "Subject: two\r\n\r\nbody {5}\r\n");

        ImapClient imap;
        imap.connect(settings(srv.imapPort(), QStringLiteral("bob@x.test"), QStringLiteral("p\"w")));
        const auto mb = imap.select();
        QCOMPARE(mb.uidValidity, 777u);
        QCOMPARE(mb.exists, 2u);
        const QList<quint32> uids = imap.uids();
        QCOMPARE(uids, (QList<quint32>{1, 2}));
        QCOMPARE(imap.fetch(2), QByteArray("Subject: two\r\n\r\nbody {5}\r\n"));
        imap.remove({1});
        QCOMPARE(imap.uids(), QList<quint32>{2});
        imap.logout();
        QCOMPARE(srv.mailbox(QStringLiteral("bob@x.test")).size(), 1);
    }

    void imapIdleWakesOnDelivery()
    {
        FakeMailServer srv;
        srv.addUser(QStringLiteral("bob@x.test"), QStringLiteral("pw"));
        ImapClient imap;
        imap.connect(settings(srv.imapPort(), QStringLiteral("bob@x.test")));
        imap.select();
        QTimer::singleShot(200, &srv, [&] { srv.deliver(QStringLiteral("bob@x.test"), "Subject: new\r\n\r\nx\r\n"); });
        QElapsedTimer t;
        t.start();
        QVERIFY(imap.idle(10000, {}));
        QVERIFY(t.elapsed() < 5000);
        // and stop() interrupts an idle with nothing happening
        QVERIFY(!imap.idle(10000, [] { return true; }));
        imap.logout();
    }

    void imapBadPassword()
    {
        FakeMailServer srv;
        srv.addUser(QStringLiteral("bob@x.test"), QStringLiteral("pw"));
        ImapClient imap;
        QVERIFY_THROWS_EXCEPTION(AuthError, imap.connect(settings(srv.imapPort(), QStringLiteral("bob@x.test"), QStringLiteral("x"))));
    }

    void pop3_data()
    {
        QTest::addColumn<bool>("sasl");
        QTest::newRow("user/pass") << false;
        QTest::newRow("sasl plain") << true;
    }

    void pop3()
    {
        QFETCH(bool, sasl);
        FakeMailServer srv;
        srv.pop3SaslPlain = sasl;
        srv.addUser(QStringLiteral("carol@x.test"), QStringLiteral("pw"));
        srv.deliver(QStringLiteral("carol@x.test"), "Subject: a\r\n\r\n.dot\r\n");
        srv.deliver(QStringLiteral("carol@x.test"), "Subject: b\r\n\r\nb\r\n");

        Pop3Client pop;
        pop.connect(settings(srv.pop3Port(), QStringLiteral("carol@x.test")));
        const auto entries = pop.list();
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries[0].uidl, QStringLiteral("uid-1"));
        QCOMPARE(pop.retrieve(1), QByteArray("Subject: a\r\n\r\n.dot\r\n"));
        pop.remove(1);
        pop.quit();
        QCOMPARE(srv.mailbox(QStringLiteral("carol@x.test")).size(), 1);

        // A dropped session must not delete anything.
        {
            Pop3Client again;
            again.connect(settings(srv.pop3Port(), QStringLiteral("carol@x.test")));
            again.remove(1);
        }
        QTest::qWait(50);
        QCOMPARE(srv.mailbox(QStringLiteral("carol@x.test")).size(), 1);
    }

    void dotStuffing()
    {
        QCOMPARE(SmtpClient::prepareData("a\n.b\r\n..c"), QByteArray("a\r\n..b\r\n...c\r\n.\r\n"));
    }
};

QTEST_GUILESS_MAIN(TestNet)
#include "tst_net.moc"
