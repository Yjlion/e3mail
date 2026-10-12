// SPDX-License-Identifier: MPL-2.0
#include "FakeMailServer.h"
#include "net/Imap.h"
#include "net/Pop3.h"
#include "net/Smtp.h"
#include "net/SyncSocket.h"

#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QtTest>

// A TLS endpoint with the self-signed certificate in tests/data: it starts
// TLS at once and then says "hello". With STARTTLS the client connects in
// cleartext first and asks for TLS, which here simply follows.
class TlsServer : public QTcpServer
{
public:
    TlsServer()
    {
        QFile c(QStringLiteral(E3_TEST_DATA "/tls-cert.pem"));
        QFile k(QStringLiteral(E3_TEST_DATA "/tls-key.pem"));
        if (c.open(QIODevice::ReadOnly) && k.open(QIODevice::ReadOnly)) {
            cert = QSslCertificate(c.readAll());
            key = QSslKey(k.readAll(), QSsl::Rsa);
        }
        listen(QHostAddress::LocalHost);
    }
    QSslCertificate cert;
    QSslKey key;

protected:
    void incomingConnection(qintptr fd) override
    {
        auto *s = new QSslSocket(this);
        s->setSocketDescriptor(fd);
        s->setLocalCertificate(cert);
        s->setPrivateKey(key);
        QObject::connect(s, &QSslSocket::encrypted, s, [s] { s->write("hello\r\n"); });
        s->startServerEncryption();
    }
};

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
        const QList<quint32> uids = imap.uids(1);
        QCOMPARE(uids, (QList<quint32>{1, 2}));
        QCOMPARE(imap.fetch(2).value_or(QByteArray()), QByteArray("Subject: two\r\n\r\nbody {5}\r\n"));
        QVERIFY(!imap.fetch(99)); // gone
        imap.remove({1});
        QCOMPARE(imap.uids(1), QList<quint32>{2});
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

    void certificatePinning_data()
    {
        QTest::addColumn<int>("security");
        QTest::newRow("implicit TLS") << int(Security::Ssl);
        QTest::newRow("STARTTLS") << int(Security::StartTls);
    }

    // ADR 0017: an untrusted certificate is refused with what is needed to
    // decide about it; once pinned it is trusted, and only it.
    void certificatePinning()
    {
        QFETCH(int, security);
        TlsServer srv;
        QVERIFY(!srv.cert.isNull());
        const QByteArray sha = srv.cert.digest(QCryptographicHash::Sha256).toHex();
        ServerSettings s;
        s.host = QStringLiteral("127.0.0.1");
        s.port = srv.serverPort();
        s.security = Security(security);
        const auto connect = [&](SyncSocket &sock) {
            sock.connectToHost(s);
            if (s.security == Security::StartTls)
                sock.startTls();
        };

        {
            SyncSocket sock(5000);
            try {
                connect(sock);
                QFAIL("an untrusted certificate was accepted");
            } catch (const CertError &e) {
                QCOMPARE(e.info.sha256, sha);
                QCOMPARE(e.info.subject, QStringLiteral("mail.e3.test"));
                QVERIFY(!e.info.problems.isEmpty());
                QVERIFY(!e.info.pinChanged);
                QVERIFY(e.info.expires.isValid());
            }
        }
        {
            s.pinnedSha256 = sha;
            SyncSocket sock(5000);
            connect(sock);
            QVERIFY(sock.isEncrypted());
            QCOMPARE(sock.readLine(), QByteArray("hello"));
        }
        {
            s.pinnedSha256 = QByteArray(64, 'a');
            SyncSocket sock(5000);
            try {
                connect(sock);
                QFAIL("a certificate other than the pinned one was accepted");
            } catch (const CertError &e) {
                QCOMPARE(e.info.sha256, sha);
                QVERIFY(e.info.pinChanged);
            }
        }
    }

    // A pin also refuses a certificate TLS would trust: it is the one
    // certificate accepted from that server.
    void pinRefusesATrustedCertificate()
    {
        TlsServer srv;
        const QSslConfiguration before = QSslConfiguration::defaultConfiguration();
        QSslConfiguration trusting = before;
        trusting.addCaCertificate(srv.cert);
        QSslConfiguration::setDefaultConfiguration(trusting);
        ServerSettings s;
        s.host = QStringLiteral("127.0.0.1");
        s.port = srv.serverPort();
        {
            SyncSocket sock(5000);
            sock.connectToHost(s);
            QCOMPARE(sock.readLine(), QByteArray("hello")); // trusted, no pin
        }
        s.pinnedSha256 = QByteArray(64, 'b');
        SyncSocket sock(5000);
        bool refused = false;
        try {
            sock.connectToHost(s);
        } catch (const CertError &e) {
            refused = e.info.pinChanged;
        }
        QSslConfiguration::setDefaultConfiguration(before);
        QVERIFY(refused);
    }

    void fingerprints()
    {
        const QByteArray sha = "a3347807083c4ebb972586cee104822f5d67eff72086cf456204b537b286d2b6";
        QCOMPARE(formatFingerprint(sha).left(11), QStringLiteral("A3:34:78:07"));
        QCOMPARE(normalizeFingerprint(formatFingerprint(sha)), sha);
        QCOMPARE(normalizeFingerprint(QStringLiteral("A334 7807 083C 4EBB 9725 86CE E104 822F 5D67 EFF7 2086 CF45 6204 B537 B286 D2B6")), sha);
        QVERIFY(normalizeFingerprint(QStringLiteral("A3:34")).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestNet)
#include "tst_net.moc"
