// SPDX-License-Identifier: MPL-2.0
#include "engine/Autoconfig.h"

#include <QSignalSpy>
#include <QTimer>
#include <QUrl>
#include <QtTest>

using namespace e3;
using Server = Autoconfig::Server;
using Result = Autoconfig::Result;
using DnsRecord = Autoconfig::DnsRecord;

namespace {

// Answers from canned maps, each after its own delay; anything not listed
// fails at once. Records what was asked, POST bodies included.
class FakeAutoconfig : public Autoconfig
{
public:
    struct Answer
    {
        QByteArray body;
        int delayMs = 0;
    };
    QHash<QString, Answer> http;                  // by URL
    QHash<QString, QList<DnsRecord>> dns;         // by name, SRV and MX alike
    QHash<QString, int> dnsDelay;
    QStringList asked;
    QHash<QString, QByteArray> posted;

    using Autoconfig::setDeadline;

protected:
    void fetch(const QUrl &url, const QByteArray &postBody,
               std::function<void(std::optional<QByteArray>)> done) override
    {
        const QString u = url.toString();
        asked.append(u);
        if (!postBody.isEmpty())
            posted.insert(u, postBody);
        const auto it = http.constFind(u);
        if (it == http.cend()) {
            QTimer::singleShot(0, this, [done] { done(std::nullopt); });
            return;
        }
        const QByteArray body = it->body;
        QTimer::singleShot(it->delayMs, this, [done, body] { done(body); });
    }
    void resolve(DnsType, const QString &name, std::function<void(QList<DnsRecord>)> done) override
    {
        asked.append(name);
        const QList<DnsRecord> recs = dns.value(name);
        QTimer::singleShot(dnsDelay.value(name), this, [done, recs] { done(recs); });
    }
};

QByteArray autoconfigXml(const QString &imapHost)
{
    return QStringLiteral(R"(<?xml version="1.0"?>
<clientConfig version="1.1"><emailProvider id="x.test">
  <incomingServer type="pop3"><hostname>pop.x.test</hostname><port>995</port>
    <socketType>SSL</socketType><username>%EMAILLOCALPART%</username></incomingServer>
  <incomingServer type="imap"><hostname>%1</hostname><port>993</port>
    <socketType>SSL</socketType><username>%EMAILADDRESS%</username></incomingServer>
  <incomingServer type="imap"><hostname>plain.x.test</hostname><port>143</port>
    <socketType>plain</socketType><username>%EMAILADDRESS%</username></incomingServer>
  <outgoingServer type="smtp"><hostname>smtp.x.test</hostname><port>587</port>
    <socketType>STARTTLS</socketType><username>%EMAILADDRESS%</username></outgoingServer>
</emailProvider></clientConfig>)")
        .arg(imapHost)
        .toUtf8();
}

const QByteArray kAutodiscover = R"(<?xml version="1.0" encoding="utf-8"?>
<Autodiscover xmlns="http://schemas.microsoft.com/exchange/autodiscover/responseschema/2006">
 <Response xmlns="http://schemas.microsoft.com/exchange/autodiscover/outlook/responseschema/2006a">
  <Account><AccountType>email</AccountType><Action>settings</Action>
   <Protocol><Type>IMAP</Type><Server>outlook.office365.com</Server><Port>993</Port>
     <SSL>on</SSL><LoginName>me@corp.test</LoginName></Protocol>
   <Protocol><Type>POP3</Type><Server>pop.corp.test</Server><Port>110</Port><SSL>off</SSL></Protocol>
   <Protocol><Type>SMTP</Type><Server>smtp.office365.com</Server><Port>587</Port>
     <Encryption>TLS</Encryption></Protocol>
   <Protocol><Type>EXCH</Type><Server>ex.corp.test</Server></Protocol>
  </Account>
 </Response>
</Autodiscover>)";

const QByteArray kRedirect = R"(<?xml version="1.0" encoding="utf-8"?>
<Autodiscover xmlns="http://schemas.microsoft.com/exchange/autodiscover/responseschema/2006">
 <Response xmlns="http://schemas.microsoft.com/exchange/autodiscover/outlook/responseschema/2006a">
  <Account><Action>redirectAddr</Action><RedirectAddr>me@corp.test</RedirectAddr></Account>
 </Response>
</Autodiscover>)";

const QString kIspdb = QStringLiteral("https://autoconfig.thunderbird.net/v1.1/");

void addSrv(FakeAutoconfig &f, const QString &domain, const QString &host)
{
    f.dns.insert(QStringLiteral("_imaps._tcp.") + domain, {{host, 993, 0, 0}});
    f.dns.insert(QStringLiteral("_submissions._tcp.") + domain, {{host, 465, 0, 0}});
}

Result run(FakeAutoconfig &f, const QString &addr)
{
    QSignalSpy spy(&f, &Autoconfig::finished);
    f.lookup(addr);
    if (spy.isEmpty())
        spy.wait(5000);
    if (spy.size() != 1)
        qFatal("expected one result, got %d", int(spy.size()));
    return spy.first().first().value<Result>();
}

} // namespace

class TestAutoconfig : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { qRegisterMetaType<Result>(); }

    void parsesAutoconfig()
    {
        const Result r = Autoconfig::parseAutoconfig(autoconfigXml(QStringLiteral("imap.x.test")),
                                                     QStringLiteral("me@x.test"));
        // IMAP first, and the cleartext one is never offered.
        QCOMPARE(r.incoming.size(), 2);
        QCOMPARE(r.incoming[0].protocol, QStringLiteral("imap"));
        QCOMPARE(r.incoming[0].host, QStringLiteral("imap.x.test"));
        QCOMPARE(r.incoming[0].username, QStringLiteral("me@x.test"));
        QCOMPARE(r.incoming[1].username, QStringLiteral("me"));
        QCOMPARE(r.smtp.port, quint16(587));
        QCOMPARE(r.smtp.security, net::Security::StartTls);
        QVERIFY(r.complete());
    }

    void parsesAutodiscover()
    {
        const Result r = Autoconfig::parseAutodiscover(kAutodiscover, QStringLiteral("other@corp.test"));
        QCOMPARE(r.incoming.size(), 1); // POP3 without TLS is dropped
        QCOMPARE(r.incoming[0].host, QStringLiteral("outlook.office365.com"));
        QCOMPARE(r.incoming[0].security, net::Security::Ssl);
        QCOMPARE(r.incoming[0].username, QStringLiteral("me@corp.test"));
        QCOMPARE(r.smtp.host, QStringLiteral("smtp.office365.com"));
        QCOMPARE(r.smtp.security, net::Security::StartTls);
        QCOMPARE(r.smtp.username, QStringLiteral("other@corp.test")); // no LoginName

        QString to;
        QVERIFY(!Autoconfig::parseAutodiscover(kRedirect, QStringLiteral("a@b.test"), &to).complete());
        QCOMPARE(to, QStringLiteral("me@corp.test"));

        // The request carries the address, escaped.
        QVERIFY(Autoconfig::autodiscoverRequest(QStringLiteral("a<b>@c.test")).contains("a&lt;b&gt;@c.test"));
    }

    void readsSrv()
    {
        QHash<QString, QList<DnsRecord>> recs;
        recs.insert(QStringLiteral("_imaps._tcp"), {{QStringLiteral("backup.x.test."), 993, 20, 0},
                                                    {QStringLiteral("light.x.test."), 993, 10, 1},
                                                    {QStringLiteral("heavy.x.test."), 993, 10, 9}});
        recs.insert(QStringLiteral("_imap._tcp"), {{QStringLiteral("starttls.x.test"), 143, 0, 0}});
        recs.insert(QStringLiteral("_pop3s._tcp"), {{QStringLiteral("."), 0, 0, 0}}); // not offered
        recs.insert(QStringLiteral("_submission._tcp"), {{QStringLiteral("smtp.x.test."), 587, 0, 0}});
        const Result r = Autoconfig::fromSrv(recs, QStringLiteral("me@x.test"));
        QCOMPARE(r.incoming.size(), 1);
        QCOMPARE(r.incoming[0].host, QStringLiteral("heavy.x.test")); // priority, then weight; implicit TLS
        QCOMPARE(r.incoming[0].security, net::Security::Ssl);
        QCOMPARE(r.smtp.host, QStringLiteral("smtp.x.test"));
        QCOMPARE(r.smtp.port, quint16(587));
        QCOMPARE(r.smtp.security, net::Security::StartTls);
        QCOMPARE(r.smtp.username, QStringLiteral("me@x.test"));
    }

    void mxDomains()
    {
        QCOMPARE(Autoconfig::mxDomains(QStringLiteral("aspmx.l.google.com.")),
                 (QStringList{QStringLiteral("l.google.com"), QStringLiteral("google.com")}));
        QCOMPARE(Autoconfig::mxDomains(QStringLiteral("localhost")), QStringList());
    }

    void providerBeatsTheRest()
    {
        FakeAutoconfig f;
        f.http.insert(QStringLiteral("https://autoconfig.x.test/mail/config-v1.1.xml?emailaddress=me@x.test"),
                      {autoconfigXml(QStringLiteral("own.x.test")), 50});
        f.http.insert(kIspdb + QStringLiteral("x.test"), {autoconfigXml(QStringLiteral("ispdb.x.test"))});
        addSrv(f, QStringLiteral("x.test"), QStringLiteral("srv.x.test"));
        const Result r = run(f, QStringLiteral("me@x.test"));
        QCOMPARE(r.source, QStringLiteral("provider"));
        QCOMPARE(r.incoming[0].host, QStringLiteral("own.x.test"));
        QVERIFY(!r.needsReview);
    }

    void waitsForWhatIsMoreTrusted()
    {
        FakeAutoconfig f;
        f.http.insert(QStringLiteral("https://x.test/.well-known/autoconfig/mail/config-v1.1.xml"),
                      {"<clientConfig/>"});
        f.http.insert(kIspdb + QStringLiteral("x.test"), {autoconfigXml(QStringLiteral("ispdb.x.test")), 200});
        addSrv(f, QStringLiteral("x.test"), QStringLiteral("srv.x.test"));
        const Result r = run(f, QStringLiteral("me@x.test"));
        QCOMPARE(r.source, QStringLiteral("ispdb"));
    }

    void dnsWhenNothingElse()
    {
        FakeAutoconfig f;
        addSrv(f, QStringLiteral("x.test"), QStringLiteral("mail.x.test"));
        Result r = run(f, QStringLiteral("me@x.test"));
        QCOMPARE(r.source, QStringLiteral("srv"));
        QCOMPARE(r.incoming[0].host, QStringLiteral("mail.x.test"));
        QVERIFY(!r.needsReview);

        // Hosts outside the domain, from unauthenticated DNS, are shown first.
        FakeAutoconfig g;
        addSrv(g, QStringLiteral("x.test"), QStringLiteral("mail.elsewhere.test"));
        r = run(g, QStringLiteral("me@x.test"));
        QCOMPARE(r.source, QStringLiteral("srv"));
        QVERIFY(r.needsReview);
    }

    void autodiscoverFollowsOneRedirect()
    {
        FakeAutoconfig f;
        const QString path = QStringLiteral("/autodiscover/autodiscover.xml");
        f.http.insert(QStringLiteral("https://autodiscover.x.test") + path, {kRedirect});
        f.http.insert(QStringLiteral("https://autodiscover.corp.test") + path, {kAutodiscover});
        const Result r = run(f, QStringLiteral("me@x.test"));
        QCOMPARE(r.source, QStringLiteral("autodiscover"));
        QCOMPARE(r.incoming[0].username, QStringLiteral("me@corp.test"));
        QVERIFY(f.posted.value(QStringLiteral("https://autodiscover.x.test") + path).contains("me@x.test"));
        QVERIFY(f.posted.value(QStringLiteral("https://autodiscover.corp.test") + path).contains("me@corp.test"));
    }

    void autodiscoverSrvTarget()
    {
        FakeAutoconfig f;
        f.dns.insert(QStringLiteral("_autodiscover._tcp.x.test"), {{QStringLiteral("ad.host.test."), 443, 0, 0}});
        f.http.insert(QStringLiteral("https://ad.host.test/autodiscover/autodiscover.xml"), {kAutodiscover});
        QCOMPARE(run(f, QStringLiteral("me@x.test")).source, QStringLiteral("autodiscover"));
    }

    void mailExchangerAsksIspdb()
    {
        FakeAutoconfig f;
        f.dns.insert(QStringLiteral("x.test"), {{QStringLiteral("mx2.mailhost.test."), 0, 20, 0},
                                                {QStringLiteral("mx1.mailhost.test."), 0, 10, 0}});
        f.http.insert(kIspdb + QStringLiteral("mailhost.test"), {autoconfigXml(QStringLiteral("imap.mailhost.test"))});
        const Result r = run(f, QStringLiteral("me@x.test"));
        QCOMPARE(r.source, QStringLiteral("mx"));
        QVERIFY(r.needsReview);
    }

    void guessesLast()
    {
        FakeAutoconfig f;
        const Result r = run(f, QStringLiteral("me@x.test"));
        QCOMPARE(r.source, QStringLiteral("guess"));
        QCOMPARE(r.incoming[0].host, QStringLiteral("imap.x.test"));
        // Nothing is sent but the address to its own domain's hosts and the
        // domain to ISPDB.
        for (const QString &u : std::as_const(f.asked))
            QVERIFY2(u.contains(QStringLiteral("x.test")), qPrintable(u));
    }

    void deadline()
    {
        FakeAutoconfig f;
        f.setDeadline(100);
        f.http.insert(kIspdb + QStringLiteral("x.test"), {autoconfigXml(QStringLiteral("late.x.test")), 3000});
        addSrv(f, QStringLiteral("x.test"), QStringLiteral("mail.x.test"));
        QElapsedTimer t;
        t.start();
        const Result r = run(f, QStringLiteral("me@x.test"));
        QCOMPARE(r.source, QStringLiteral("srv"));
        QVERIFY(t.elapsed() < 2000);
    }

    void newLookupAbandonsTheOld()
    {
        FakeAutoconfig f;
        f.http.insert(kIspdb + QStringLiteral("x.test"), {autoconfigXml(QStringLiteral("old.x.test")), 100});
        f.http.insert(kIspdb + QStringLiteral("y.test"), {autoconfigXml(QStringLiteral("new.y.test")), 200});
        QSignalSpy spy(&f, &Autoconfig::finished);
        f.lookup(QStringLiteral("me@x.test"));
        f.lookup(QStringLiteral("me@y.test"));
        QVERIFY(spy.wait(2000));
        QTest::qWait(200);
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.first().first().value<Result>().incoming[0].host, QStringLiteral("new.y.test"));
    }
};

QTEST_GUILESS_MAIN(TestAutoconfig)
#include "tst_autoconfig.moc"
