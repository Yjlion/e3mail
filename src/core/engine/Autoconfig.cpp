// SPDX-License-Identifier: MPL-2.0
#include "Autoconfig.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>
#include <QXmlStreamReader>

namespace e3 {

namespace {
QString expand(QString s, const QString &addr)
{
    s.replace(QLatin1String("%EMAILADDRESS%"), addr);
    s.replace(QLatin1String("%EMAILLOCALPART%"), addr.section(u'@', 0, 0));
    s.replace(QLatin1String("%EMAILDOMAIN%"), addr.section(u'@', 1));
    return s;
}
} // namespace

Autoconfig::Autoconfig(QObject *parent) : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

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

Autoconfig::Result Autoconfig::parseIspdb(const QByteArray &xml, const QString &addr)
{
    Result r;
    r.source = QStringLiteral("ispdb");
    QXmlStreamReader x(xml);
    Server cur;
    bool inServer = false;
    bool outgoing = false;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            const auto name = x.name();
            if (name == QLatin1String("incomingServer") || name == QLatin1String("outgoingServer")) {
                inServer = true;
                outgoing = name == QLatin1String("outgoingServer");
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
            // Never suggest a cleartext connection.
            if (cur.security == net::Security::Plain)
                continue;
            if (outgoing && r.smtp.host.isEmpty())
                r.smtp = cur;
            else if (!outgoing && (cur.protocol == QLatin1String("imap") || cur.protocol == QLatin1String("pop3")))
                r.incoming.append(cur);
        }
    }
    std::stable_sort(r.incoming.begin(), r.incoming.end(),
                     [](const Server &a, const Server &b) { return a.protocol == QLatin1String("imap") && b.protocol != a.protocol; });
    return r;
}

void Autoconfig::lookup(const QString &addr)
{
    const QString domain = addr.section(u'@', 1).toLower();
    if (domain.isEmpty()) {
        Q_EMIT finished(guess(addr));
        return;
    }
    QNetworkRequest req(QUrl(QStringLiteral("https://autoconfig.thunderbird.net/v1.1/") + domain));
    req.setTransferTimeout(8000);
    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, addr] {
        reply->deleteLater();
        Result r;
        if (reply->error() == QNetworkReply::NoError)
            r = parseIspdb(reply->readAll(), addr);
        if (r.incoming.isEmpty() || r.smtp.host.isEmpty())
            r = guess(addr);
        Q_EMIT finished(r);
    });
}

} // namespace e3
