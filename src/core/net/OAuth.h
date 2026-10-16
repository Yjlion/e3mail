// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QDateTime>
#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

#include <functional>
#include <optional>

// OAuth 2 for Google and Microsoft mail (ADR 0019). The core knows the
// providers and refreshes access tokens, blocking on the calling thread (the
// mail worker's); signing in with a browser is e3oauthflow's (src/oauthflow).
namespace e3::net::oauth {

struct Provider
{
    QString id;     // "google" | "microsoft": the account's `auth` is "oauth:" + id
    QString name;   // for "Sign in with %1"
    QString issuer; // as ISPDB's <oAuth2><issuer> names it
    QUrl authUrl;
    QUrl tokenUrl;
    QStringList scopes;
    QString clientId;
    QString clientSecret; // Google's for desktop apps, which it says is not secret
    QVariantMap authParams; // extra parameters for the authorization request
};

// A provider with the client ID this build carries, or one from the
// environment (E3MAIL_OAUTH_GOOGLE_ID, E3MAIL_OAUTH_GOOGLE_SECRET,
// E3MAIL_OAUTH_MICROSOFT_ID) for people building their own. Without an ID,
// nothing: that provider is offered nowhere.
std::optional<Provider> provider(const QString &id);
// The same, with or without a client ID (an account's own is stored with it).
std::optional<Provider> known(const QString &id);
std::optional<Provider> forIssuer(const QString &issuer);
// The provider for a mail domain, by name, before any lookup: gmail.com,
// outlook.com and the like.
std::optional<Provider> forDomain(const QString &domain);

// The email address in an OpenID Connect ID token, read without checking its
// signature: it came straight from the provider's token endpoint over TLS.
QString emailFromIdToken(const QString &jwt);

// An account's tokens. Thread-safe; accessToken() blocks while it refreshes.
class TokenSource
{
public:
    TokenSource(Provider provider, QString refreshToken);

    // A token valid for at least another minute, refreshed when needed or
    // when `fresh`. Throws AuthError when the refresh token no longer works
    // (revoked, expired, password changed: the person must sign in again),
    // NetError when the provider cannot be reached.
    QString accessToken(bool fresh = false);
    QString refreshToken() const;
    // Called, on the refreshing thread, when the provider issues a new
    // refresh token (Microsoft rotates them); it must be stored.
    std::function<void(const QString &)> onRefreshTokenChanged;

    // For tests and a first sign-in: a token known to be fresh.
    void setAccessToken(const QString &token, const QDateTime &expires);

private:
    mutable QMutex m_mutex;
    Provider m_provider;
    QString m_refresh;
    QString m_access;
    QDateTime m_expires;
};

} // namespace e3::net::oauth
