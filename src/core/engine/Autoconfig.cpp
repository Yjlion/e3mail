// SPDX-License-Identifier: MPL-2.0
#include "Autoconfig.h"

#include <QDnsLookup>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <algorithm>
#include <memory>

namespace e3 {

namespace {

constexpr int kRequestTimeoutMs = 8000;
const QString kIspdb = QStringLiteral("https://autoconfig.thunderbird.net/v1.1/");

QString expand(QString s, const QString &addr)
{
    s.replace(QLatin1String("%EMAILADDRESS%"), addr);
    s.replace(QLatin1String("%EMAILLOCALPART%"), addr.section(u'@', 0, 0));
    s.replace(QLatin1String("%EMAILDOMAIN%"), addr.section(u'@', 1));
    return s;
}

void sortIncoming(QList<Autoconfig::Server> &incoming)
{
    std::stable_sort(incoming.begin(), incoming.end(), [](const Autoconfig::Server &a, const Autoconfig::Server &b) {
        return a.protocol == QLatin1String("imap") && b.protocol != a.protocol;
    });
}

// Adds a server to the result unless it is cleartext or the slot is taken.
void take(Autoconfig::Result &r, const Autoconfig::Server &s)
{
    if (s.host.isEmpty() || s.port == 0 || s.security == net::Security::Plain)
        return;
    if (s.protocol == QLatin1String("smtp")) {
        if (r.smtp.host.isEmpty())
            r.smtp = s;
    } else if (s.protocol == QLatin1String("imap") || s.protocol == QLatin1String("pop3")) {
        r.incoming.append(s);
    }
}

bool implicitTlsPort(quint16 port)
{
    return port == 993 || port == 995 || port == 465;
}

quint16 defaultPort(const QString &protocol, bool implicitTls)
{
    if (protocol == QLatin1String("imap"))
        return implicitTls ? 993 : 143;
    if (protocol == QLatin1String("pop3"))
        return implicitTls ? 995 : 110;
    return implicitTls ? 465 : 587;
}

QString stripDot(QString host)
{
    while (host.endsWith(u'.'))
        host.chop(1);
    return host.toLower();
}

bool inDomain(const QString &host, const QString &domain)
{
    return host == domain || host.endsWith(u'.' + domain);
}

} // namespace

Autoconfig::Autoconfig(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)), m_deadline(new QTimer(this))
{
    m_deadline->setSingleShot(true);
    connect(m_deadline, &QTimer::timeout, this, [this] { settle(true); });
}

Autoconfig::~Autoconfig() = default;

Autoconfig::Result Autoconfig::guess(const QString &addr)
{
    const QString domain = addr.section(u'@', 1).toLower();
    Result r;
    r.source = QStringLiteral("guess");
    r.incoming.append({QStringLiteral("imap"), QStringLiteral("imap.") + domain, 993, net::Security::Ssl, addr});
    r.incoming.append({QStringLiteral("pop3"), QStringLiteral("pop.") + domain, 995, net::Security::Ssl, addr});
    r.smtp = {QStringLiteral("smtp"), QStringLiteral("smtp.") + domain, 465, net::Security::Ssl, addr};
    return r;
}

Autoconfig::Result Autoconfig::parseAutoconfig(const QByteArray &xml, const QString &addr)
{
    Result r;
    QXmlStreamReader x(xml);
    Server cur;
    bool inServer = false;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            const auto name = x.name();
            if (name == QLatin1String("incomingServer") || name == QLatin1String("outgoingServer")) {
                inServer = true;
                cur = Server();
                cur.protocol = x.attributes().value(QLatin1String("type")).toString();
            } else if (inServer && name == QLatin1String("hostname")) {
                cur.host = expand(x.readElementText(), addr);
            } else if (inServer && name == QLatin1String("port")) {
                cur.port = quint16(x.readElementText().toUInt());
            } else if (inServer && name == QLatin1String("socketType")) {
                const QString t = x.readElementText();
                cur.security = t == QLatin1String("SSL") ? net::Security::Ssl
                    : t == QLatin1String("STARTTLS")     ? net::Security::StartTls
                                                         : net::Security::Plain;
            } else if (inServer && name == QLatin1String("username")) {
                cur.username = expand(x.readElementText(), addr);
            }
        } else if (x.isEndElement() && inServer
                   && (x.name() == QLatin1String("incomingServer") || x.name() == QLatin1String("outgoingServer"))) {
            inServer = false;
            take(r, cur);
        }
    }
    sortIncoming(r.incoming);
    return r;
}

QByteArray Autoconfig::autodiscoverRequest(const QString &addr)
{
    return QStringLiteral(
               "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
               "<Autodiscover xmlns=\"http://schemas.microsoft.com/exchange/autodiscover/outlook/requestschema/2006\">"
               "<Request><EMailAddress>%1</EMailAddress>"
               "<AcceptableResponseSchema>http://schemas.microsoft.com/exchange/autodiscover/outlook/responseschema/"
               "2006a</AcceptableResponseSchema></Request></Autodiscover>\r\n")
        .arg(addr.toHtmlEscaped())
        .toUtf8();
}

Autoconfig::Result Autoconfig::parseAutodiscover(const QByteArray &xml, const QString &addr, QString *redirectAddr)
{
    Result r;
    QXmlStreamReader x(xml);
    struct Proto
    {
        QString type, server, ssl, encryption, login;
        quint16 port = 0;
    } cur;
    int depth = 0; // inside a <Protocol>
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            const auto name = x.name();
            if (name == QLatin1String("Protocol")) {
                if (depth++ == 0) {
                    cur = Proto();
                    cur.type = x.attributes().value(QLatin1String("Type")).toString();
                }
            } else if (depth == 1 && name == QLatin1String("Type")) {
                cur.type = x.readElementText().trimmed();
            } else if (depth == 1 && name == QLatin1String("Server")) {
                cur.server = x.readElementText().trimmed();
            } else if (depth == 1 && name == QLatin1String("Port")) {
                cur.port = quint16(x.readElementText().trimmed().toUInt());
            } else if (depth == 1 && name == QLatin1String("SSL")) {
                cur.ssl = x.readElementText().trimmed().toLower();
            } else if (depth == 1 && name == QLatin1String("Encryption")) {
                cur.encryption = x.readElementText().trimmed().toLower();
            } else if (depth == 1 && name == QLatin1String("LoginName")) {
                cur.login = x.readElementText().trimmed();
            } else if (depth == 0 && name == QLatin1String("RedirectAddr") && redirectAddr) {
                *redirectAddr = x.readElementText().trimmed();
            }
        } else if (x.isEndElement() && x.name() == QLatin1String("Protocol") && depth > 0 && --depth == 0) {
            Server s;
            s.protocol = cur.type.toLower();
            if (s.protocol == QLatin1String("pop3") || s.protocol == QLatin1String("imap")
                || s.protocol == QLatin1String("smtp")) {
                // <Encryption> overrides <SSL>, whose absence means on.
                bool tls = cur.ssl != QLatin1String("off");
                bool implicit = cur.port ? implicitTlsPort(cur.port) : tls;
                if (cur.encryption == QLatin1String("none")) {
                    tls = false;
                } else if (cur.encryption == QLatin1String("ssl")) {
                    tls = implicit = true;
                } else if (cur.encryption == QLatin1String("tls")) {
                    tls = true;
                    implicit = false;
                }
                s.host = cur.server;
                s.port = cur.port ? cur.port : defaultPort(s.protocol, implicit);
                s.security = !tls ? net::Security::Plain : implicit ? net::Security::Ssl : net::Security::StartTls;
                s.username = cur.login.isEmpty() ? addr : cur.login;
                take(r, s);
            }
        }
    }
    sortIncoming(r.incoming);
    return r;
}

Autoconfig::Result Autoconfig::fromSrv(const QHash<QString, QList<DnsRecord>> &records, const QString &addr)
{
    // The best offered record of a service: a lone "." target means the
    // service is not offered (RFC 2782).
    auto pick = [&](const QString &service) -> std::optional<DnsRecord> {
        QList<DnsRecord> list;
        for (DnsRecord rec : records.value(service)) {
            rec.target = stripDot(rec.target);
            if (!rec.target.isEmpty() && rec.port)
                list.append(rec);
        }
        if (list.isEmpty())
            return std::nullopt;
        std::stable_sort(list.begin(), list.end(), [](const DnsRecord &a, const DnsRecord &b) {
            return a.priority != b.priority ? a.priority < b.priority : a.weight > b.weight;
        });
        return list.first();
    };
    Result r;
    // Implicit TLS first (RFC 8314 §5.1).
    const struct
    {
        const char *protocol, *implicit, *starttls;
    } services[] = {{"imap", "_imaps._tcp", "_imap._tcp"},
                    {"pop3", "_pop3s._tcp", "_pop3._tcp"},
                    {"smtp", "_submissions._tcp", "_submission._tcp"}};
    for (const auto &svc : services) {
        net::Security security = net::Security::Ssl;
        auto rec = pick(QLatin1String(svc.implicit));
        if (!rec) {
            rec = pick(QLatin1String(svc.starttls));
            security = net::Security::StartTls;
        }
        if (rec)
            take(r, {QLatin1String(svc.protocol), rec->target, rec->port, security, addr});
    }
    return r;
}

QStringList Autoconfig::mxDomains(const QString &mxHost)
{
    const QStringList labels = stripDot(mxHost).split(u'.', Qt::SkipEmptyParts);
    QStringList out;
    for (qsizetype i = 1; labels.size() - i >= 2; ++i)
        out.append(labels.mid(i).join(u'.'));
    return out;
}

void Autoconfig::setDeadline(int ms)
{
    m_deadlineMs = ms;
}

void Autoconfig::fetch(const QUrl &url, const QByteArray &postBody,
                       std::function<void(std::optional<QByteArray>)> done)
{
    QNetworkRequest req(url);
    req.setTransferTimeout(kRequestTimeoutMs);
    QNetworkReply *reply;
    if (postBody.isEmpty()) {
        reply = m_nam->get(req);
    } else {
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/xml; charset=utf-8"));
        reply = m_nam->post(req, postBody);
    }
    // No credentials are ever offered: a server that asks for them has
    // nothing for us (authenticationRequired has no handler, so it fails).
    connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError
            || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
            done(std::nullopt);
        else
            done(reply->readAll());
    });
}

void Autoconfig::resolve(DnsType type, const QString &name, std::function<void(QList<DnsRecord>)> done)
{
    auto *lookup = new QDnsLookup(type == DnsType::Srv ? QDnsLookup::SRV : QDnsLookup::MX, name, this);
    connect(lookup, &QDnsLookup::finished, this, [lookup, type, done = std::move(done)] {
        lookup->deleteLater();
        QList<DnsRecord> out;
        if (lookup->error() == QDnsLookup::NoError) {
            if (type == DnsType::Srv) {
                for (const QDnsServiceRecord &r : lookup->serviceRecords())
                    out.append({r.target(), r.port(), r.priority(), r.weight()});
            } else {
                for (const QDnsMailExchangeRecord &r : lookup->mailExchangeRecords())
                    out.append({r.exchange(), 0, r.preference(), 0});
            }
        }
        done(out);
    });
    lookup->lookup();
}

void Autoconfig::lookup(const QString &addr)
{
    ++m_gen;
    m_addr = addr.trimmed();
    m_domain = m_addr.section(u'@', 1).toLower();
    for (auto &r : m_results)
        r.reset();
    m_done = false;
    if (m_domain.isEmpty()) {
        settle(true);
        return;
    }
    m_deadline->start(m_deadlineMs);
    const quint64 gen = m_gen;
    startProvider(gen);
    startIspdb(gen);
    startAutodiscover(gen);
    startSrv(gen);
    startMx(gen);
}

bool Autoconfig::current(quint64 gen) const
{
    return gen == m_gen && !m_done;
}

void Autoconfig::finish(Source s, Result r)
{
    static const char *const names[SourceCount] = {"provider", "ispdb", "autodiscover", "srv", "mx"};
    r.source = QLatin1String(names[s]);
    if (s == Srv || s == Mx) {
        auto outside = [this](const Server &x) { return !inDomain(x.host.toLower(), m_domain); };
        r.needsReview = std::any_of(r.incoming.cbegin(), r.incoming.cend(), outside) || outside(r.smtp);
    }
    m_results[s] = std::move(r);
    settle(false);
}

void Autoconfig::settle(bool deadline)
{
    if (m_done)
        return;
    for (const auto &r : m_results) {
        if (!r) {
            if (deadline)
                continue;
            return; // something more trusted may still answer
        }
        if (r->complete()) {
            m_done = true;
            m_deadline->stop();
            Q_EMIT finished(*r);
            return;
        }
    }
    m_done = true;
    m_deadline->stop();
    Q_EMIT finished(guess(m_addr));
}

void Autoconfig::fetchFirst(quint64 gen, const QList<QUrl> &urls, const QByteArray &postBody,
                            std::function<Result(const QByteArray &)> parse, std::function<void(Result)> done)
{
    struct State
    {
        QList<std::optional<Result>> answers;
        bool reported = false;
    };
    auto st = std::make_shared<State>();
    st->answers.resize(urls.size());
    auto check = [st, done] {
        if (st->reported)
            return;
        for (const auto &a : std::as_const(st->answers)) {
            if (!a)
                return;
            if (a->complete()) {
                st->reported = true;
                done(*a);
                return;
            }
        }
        st->reported = true;
        done(Result());
    };
    if (urls.isEmpty()) {
        check();
        return;
    }
    for (qsizetype i = 0; i < urls.size(); ++i) {
        fetch(urls[i], postBody, [this, gen, st, i, parse, check](std::optional<QByteArray> body) {
            if (!current(gen))
                return;
            st->answers[i] = body ? parse(*body) : Result();
            check();
        });
    }
}

void Autoconfig::startProvider(quint64 gen)
{
    QUrl own(QStringLiteral("https://autoconfig.%1/mail/config-v1.1.xml").arg(m_domain));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("emailaddress"), m_addr);
    own.setQuery(q);
    const QUrl wellKnown(QStringLiteral("https://%1/.well-known/autoconfig/mail/config-v1.1.xml").arg(m_domain));
    const QString addr = m_addr;
    fetchFirst(gen, {own, wellKnown}, {}, [addr](const QByteArray &xml) { return parseAutoconfig(xml, addr); },
               [this](Result r) { finish(Provider, std::move(r)); });
}

void Autoconfig::startIspdb(quint64 gen)
{
    const QString addr = m_addr;
    fetchFirst(gen, {QUrl(kIspdb + m_domain)}, {}, [addr](const QByteArray &xml) { return parseAutoconfig(xml, addr); },
               [this](Result r) { finish(Ispdb, std::move(r)); });
}

void Autoconfig::startAutodiscover(quint64 gen)
{
    // The domain's own hosts, then wherever its _autodiscover SRV record
    // points (hosted Exchange). HTTPS only: the plain-HTTP redirect step of
    // the Autodiscover protocol is left out.
    resolve(DnsType::Srv, QStringLiteral("_autodiscover._tcp.") + m_domain, [this, gen](QList<DnsRecord> srv) {
        if (!current(gen))
            return;
        const QString path = QStringLiteral("/autodiscover/autodiscover.xml");
        QList<QUrl> urls{QUrl(QStringLiteral("https://autodiscover.") + m_domain + path),
                         QUrl(QStringLiteral("https://") + m_domain + path)};
        std::stable_sort(srv.begin(), srv.end(),
                         [](const DnsRecord &a, const DnsRecord &b) { return a.priority < b.priority; });
        for (const DnsRecord &r : std::as_const(srv)) {
            const QString host = stripDot(r.target);
            if (!host.isEmpty() && r.port == 443) {
                urls.append(QUrl(QStringLiteral("https://") + host + path));
                break;
            }
        }
        const QString addr = m_addr;
        auto redirect = std::make_shared<QString>();
        fetchFirst(
            gen, urls, autodiscoverRequest(addr),
            [addr, redirect](const QByteArray &xml) {
                QString to;
                Result r = parseAutodiscover(xml, addr, &to);
                if (redirect->isEmpty())
                    *redirect = to;
                return r;
            },
            [this, gen, redirect, path](Result r) {
                // One redirect to another address, no more.
                const QString to = redirect->trimmed();
                if (r.complete() || !to.contains(u'@') || to.compare(m_addr, Qt::CaseInsensitive) == 0) {
                    finish(Autodiscover, std::move(r));
                    return;
                }
                const QUrl url(QStringLiteral("https://autodiscover.") + to.section(u'@', 1).toLower() + path);
                fetchFirst(gen, {url}, autodiscoverRequest(to),
                           [to](const QByteArray &xml) { return parseAutodiscover(xml, to); },
                           [this](Result r2) { finish(Autodiscover, std::move(r2)); });
            });
    });
}

void Autoconfig::startSrv(quint64 gen)
{
    static const char *const services[] = {"_imaps._tcp", "_imap._tcp", "_pop3s._tcp",
                                           "_pop3._tcp", "_submissions._tcp", "_submission._tcp"};
    struct State
    {
        QHash<QString, QList<DnsRecord>> records;
        int pending = int(std::size(services));
    };
    auto st = std::make_shared<State>();
    for (const char *service : services) {
        const QString name = QLatin1String(service);
        resolve(DnsType::Srv, name + u'.' + m_domain, [this, gen, st, name](QList<DnsRecord> recs) {
            if (!current(gen))
                return;
            st->records.insert(name, recs);
            if (--st->pending == 0)
                finish(Srv, fromSrv(st->records, m_addr));
        });
    }
}

void Autoconfig::startMx(quint64 gen)
{
    resolve(DnsType::Mx, m_domain, [this, gen](QList<DnsRecord> mx) {
        if (!current(gen))
            return;
        std::stable_sort(mx.begin(), mx.end(),
                         [](const DnsRecord &a, const DnsRecord &b) { return a.priority < b.priority; });
        QList<QUrl> urls;
        if (!mx.isEmpty()) {
            for (const QString &d : mxDomains(mx.first().target)) {
                // The domain itself (and anything above it) ISPDB was asked about already.
                if (!inDomain(m_domain, d))
                    urls.append(QUrl(kIspdb + d));
            }
        }
        const QString addr = m_addr;
        fetchFirst(gen, urls, {}, [addr](const QByteArray &xml) { return parseAutoconfig(xml, addr); },
                   [this](Result r) { finish(Mx, std::move(r)); });
    });
}

} // namespace e3
