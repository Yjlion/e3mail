// SPDX-License-Identifier: MPL-2.0
#include "OAuth.h"
#include "SyncSocket.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QUrlQuery>

#ifndef E3MAIL_OAUTH_GOOGLE_ID
#define E3MAIL_OAUTH_GOOGLE_ID ""
#endif
#ifndef E3MAIL_OAUTH_GOOGLE_SECRET
#define E3MAIL_OAUTH_GOOGLE_SECRET ""
#endif
#ifndef E3MAIL_OAUTH_MICROSOFT_ID
#define E3MAIL_OAUTH_MICROSOFT_ID ""
#endif

namespace e3::net::oauth {

namespace {

QString fromEnvOr(const char *env, const char *built)
{
    const QString v = qEnvironmentVariable(env);
    return v.isEmpty() ? QString::fromUtf8(built) : v;
}

Provider google()
{
    Provider p;
    p.id = QStringLiteral("google");
    p.name = QStringLiteral("Google");
    p.issuer = QStringLiteral("accounts.google.com");
    p.authUrl = QUrl(QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth"));
    p.tokenUrl = QUrl(QStringLiteral("https://oauth2.googleapis.com/token"));
    p.scopes = {QStringLiteral("https://mail.google.com/"), QStringLiteral("openid"), QStringLiteral("email")};
    p.clientId = fromEnvOr("E3MAIL_OAUTH_GOOGLE_ID", E3MAIL_OAUTH_GOOGLE_ID);
    p.clientSecret = fromEnvOr("E3MAIL_OAUTH_GOOGLE_SECRET", E3MAIL_OAUTH_GOOGLE_SECRET);
    // A refresh token, every time: without prompt=consent Google gives one
    // only at the first sign-in of this app on that Google account.
    p.authParams = {{QStringLiteral("access_type"), QStringLiteral("offline")},
                    {QStringLiteral("prompt"), QStringLiteral("consent")}};
    return p;
}

Provider microsoft()
{
    Provider p;
    p.id = QStringLiteral("microsoft");
    p.name = QStringLiteral("Microsoft");
    p.issuer = QStringLiteral("login.microsoftonline.com");
    p.authUrl = QUrl(QStringLiteral("https://login.microsoftonline.com/common/oauth2/v2.0/authorize"));
    p.tokenUrl = QUrl(QStringLiteral("https://login.microsoftonline.com/common/oauth2/v2.0/token"));
    p.scopes = {QStringLiteral("https://outlook.office.com/IMAP.AccessAsUser.All"),
                QStringLiteral("https://outlook.office.com/POP.AccessAsUser.All"),
                QStringLiteral("https://outlook.office.com/SMTP.Send"), QStringLiteral("offline_access"),
                QStringLiteral("openid"), QStringLiteral("email")};
    p.clientId = fromEnvOr("E3MAIL_OAUTH_MICROSOFT_ID", E3MAIL_OAUTH_MICROSOFT_ID);
    p.authParams = {{QStringLiteral("prompt"), QStringLiteral("select_account")}};
    return p;
}

} // namespace

std::optional<Provider> known(const QString &id)
{
    if (id == QLatin1String("google"))
        return google();
    if (id == QLatin1String("microsoft"))
        return microsoft();
    return std::nullopt;
}

std::optional<Provider> provider(const QString &id)
{
    std::optional<Provider> p = known(id);
    if (!p || p->clientId.isEmpty())
        return std::nullopt;
    return p;
}

std::optional<Provider> forIssuer(const QString &issuer)
{
    const QString i = issuer.trimmed().toLower();
    if (i == QLatin1String("accounts.google.com"))
        return provider(QStringLiteral("google"));
    if (i == QLatin1String("login.microsoftonline.com") || i == QLatin1String("login.live.com"))
        return provider(QStringLiteral("microsoft"));
    return std::nullopt;
}

std::optional<Provider> forDomain(const QString &domain)
{
    static const QStringList googleDomains = {QStringLiteral("gmail.com"), QStringLiteral("googlemail.com")};
    static const QStringList microsoftDomains = {QStringLiteral("outlook.com"), QStringLiteral("hotmail.com"),
                                                 QStringLiteral("live.com"), QStringLiteral("msn.com")};
    const QString d = domain.trimmed().toLower();
    if (googleDomains.contains(d))
        return provider(QStringLiteral("google"));
    if (microsoftDomains.contains(d) || d.startsWith(QLatin1String("outlook.")) || d.startsWith(QLatin1String("hotmail.")))
        return provider(QStringLiteral("microsoft"));
    return std::nullopt;
}

QString emailFromIdToken(const QString &jwt)
{
    const QStringList parts = jwt.split(u'.');
    if (parts.size() < 2)
        return {};
    const QByteArray payload = QByteArray::fromBase64(parts[1].toLatin1(), QByteArray::Base64UrlEncoding);
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    // Microsoft personal accounts may carry it as preferred_username only.
    QString email = o.value(QLatin1String("email")).toString();
    if (email.isEmpty())
        email = o.value(QLatin1String("preferred_username")).toString();
    return email.contains(u'@') ? email : QString();
}

TokenSource::TokenSource(Provider provider, QString refreshToken)
    : m_provider(std::move(provider)), m_refresh(std::move(refreshToken))
{
}

QString TokenSource::refreshToken() const
{
    QMutexLocker lock(&m_mutex);
    return m_refresh;
}

void TokenSource::setAccessToken(const QString &token, const QDateTime &expires)
{
    QMutexLocker lock(&m_mutex);
    m_access = token;
    m_expires = expires;
}

QString TokenSource::accessToken(bool fresh)
{
    QMutexLocker lock(&m_mutex);
    if (!fresh && !m_access.isEmpty() && QDateTime::currentDateTimeUtc().secsTo(m_expires) > 60)
        return m_access;
    if (m_refresh.isEmpty())
        throw AuthError(QStringLiteral("not signed in"));

    QUrlQuery form;
    form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    form.addQueryItem(QStringLiteral("refresh_token"), m_refresh);
    form.addQueryItem(QStringLiteral("client_id"), m_provider.clientId);
    if (!m_provider.clientSecret.isEmpty())
        form.addQueryItem(QStringLiteral("client_secret"), m_provider.clientSecret);
    QNetworkRequest req(m_provider.tokenUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    req.setTransferTimeout(30000);
    // Linear code over a local event loop (ADR 0011), on this thread.
    QNetworkAccessManager nam;
    QNetworkReply *reply = nam.post(req, form.toString(QUrl::FullyEncoded).toUtf8());
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QByteArray body = reply->readAll();
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError err = reply->error();
    const QString errText = reply->errorString();
    reply->deleteLater();

    const QJsonObject o = QJsonDocument::fromJson(body).object();
    if (http == 400 || http == 401) {
        // invalid_grant: revoked, expired, or the password changed.
        const QString e = o.value(QLatin1String("error")).toString();
        throw AuthError(QStringLiteral("%1 refused to renew the sign-in (%2). Sign in again.")
                            .arg(m_provider.name, e.isEmpty() ? QString::number(http) : e));
    }
    if (err != QNetworkReply::NoError || !o.contains(QLatin1String("access_token")))
        throw NetError(QStringLiteral("%1: cannot renew the sign-in: %2").arg(m_provider.name, errText));
    m_access = o.value(QLatin1String("access_token")).toString();
    m_expires = QDateTime::currentDateTimeUtc().addSecs(o.value(QLatin1String("expires_in")).toInt(3600));
    const QString rotated = o.value(QLatin1String("refresh_token")).toString();
    if (!rotated.isEmpty() && rotated != m_refresh) {
        m_refresh = rotated;
        if (onRefreshTokenChanged)
            onRefreshTokenChanged(rotated);
    }
    return m_access;
}

} // namespace e3::net::oauth
