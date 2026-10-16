// SPDX-License-Identifier: MPL-2.0
#include "FakeOAuthServer.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QUrlQuery>

FakeOAuthServer::FakeOAuthServer()
{
    listen(QHostAddress::LocalHost);
}

e3::net::oauth::Provider FakeOAuthServer::provider() const
{
    e3::net::oauth::Provider p;
    p.id = QStringLiteral("google");
    p.name = QStringLiteral("Fake");
    p.issuer = QStringLiteral("accounts.google.com");
    p.authUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/authorize").arg(serverPort()));
    p.tokenUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/token").arg(serverPort()));
    p.scopes = {QStringLiteral("https://mail.google.com/"), QStringLiteral("openid"), QStringLiteral("email")};
    p.clientId = QStringLiteral("test-client");
    p.clientSecret = QStringLiteral("test-secret");
    p.authParams = {{QStringLiteral("access_type"), QStringLiteral("offline")}};
    return p;
}

QString FakeOAuthServer::approve(const QString &codeChallenge, const QString &email)
{
    const QString code = QStringLiteral("code-%1").arg(m_next++);
    m_codes.insert(code, {codeChallenge, email});
    return code;
}

void FakeOAuthServer::incomingConnection(qintptr fd)
{
    auto *s = new QTcpSocket(this);
    s->setSocketDescriptor(fd);
    auto buf = std::make_shared<QByteArray>();
    connect(s, &QTcpSocket::readyRead, s, [this, s, buf] {
        *buf += s->readAll();
        const qsizetype end = buf->indexOf("\r\n\r\n");
        if (end < 0)
            return;
        qsizetype length = 0;
        for (const QByteArray &h : buf->left(end).split('\n')) {
            if (h.toLower().startsWith("content-length:"))
                length = h.mid(15).trimmed().toLongLong();
        }
        if (buf->size() < end + 4 + length)
            return;
        int status = 200;
        const QByteArray body = handle(buf->mid(end + 4, length), &status);
        s->write("HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK" : " Bad Request")
                 + "\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size())
                 + "\r\nConnection: close\r\n\r\n" + body);
        s->disconnectFromHost();
    });
    connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
}

QByteArray FakeOAuthServer::handle(const QByteArray &body, int *status)
{
    const QUrlQuery q(QString::fromUtf8(body));
    const QString grant = q.queryItemValue(QStringLiteral("grant_type"));
    auto bad = [status](const char *error) {
        *status = 400;
        return QJsonDocument(QJsonObject{{QStringLiteral("error"), QLatin1String(error)}}).toJson(QJsonDocument::Compact);
    };
    if (q.queryItemValue(QStringLiteral("client_id")) != QLatin1String("test-client"))
        return bad("invalid_client");
    QString email, refresh;
    if (grant == QLatin1String("authorization_code")) {
        ++exchanges;
        const Grant g = m_codes.take(q.queryItemValue(QStringLiteral("code")));
        const QByteArray verifier = q.queryItemValue(QStringLiteral("code_verifier")).toUtf8();
        const QString challenge = QString::fromLatin1(QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
                                                          .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        if (g.email.isEmpty() || challenge != g.challenge)
            return bad("invalid_grant");
        email = g.email;
        refresh = QStringLiteral("refresh-%1").arg(m_next++);
        m_refresh.insert(refresh, email);
    } else if (grant == QLatin1String("refresh_token")) {
        ++refreshes;
        const QString r = q.queryItemValue(QStringLiteral("refresh_token"));
        if (!m_refresh.contains(r))
            return bad("invalid_grant");
        email = m_refresh.value(r);
        if (rotate) {
            m_refresh.remove(r);
            refresh = QStringLiteral("refresh-%1").arg(m_next++);
            m_refresh.insert(refresh, email);
        }
    } else {
        return bad("unsupported_grant_type");
    }
    const QString access = QStringLiteral("access-%1").arg(m_next++);
    if (issued)
        issued(email, access);
    // An unsigned ID token: header.payload.signature, base64url.
    const QByteArray payload = QJsonDocument(QJsonObject{{QStringLiteral("email"), email}}).toJson(QJsonDocument::Compact);
    const QString idToken = QStringLiteral("e30.%1.sig").arg(QString::fromLatin1(
        payload.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
    QJsonObject o{{QStringLiteral("access_token"), access}, {QStringLiteral("token_type"), QStringLiteral("Bearer")},
                  {QStringLiteral("expires_in"), expiresIn}, {QStringLiteral("id_token"), idToken}};
    if (!refresh.isEmpty())
        o.insert(QStringLiteral("refresh_token"), refresh);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
