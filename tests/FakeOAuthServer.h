// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "net/OAuth.h"

#include <QHash>
#include <QTcpServer>

#include <functional>

// An OAuth 2 token endpoint over plain HTTP on 127.0.0.1, enough for e3mail's
// sign-in and refresh (ADR 0019): grant_type=authorization_code with PKCE
// (S256), and grant_type=refresh_token. Each access token it issues is handed
// to `issued`, so a FakeMailServer can accept it.
class FakeOAuthServer : public QTcpServer
{
    Q_OBJECT
public:
    FakeOAuthServer();

    // A provider pointing here, for tests.
    e3::net::oauth::Provider provider() const;

    // The code the "browser" got for this challenge, and whose email the
    // ID token names. Returns the code.
    QString approve(const QString &codeChallenge, const QString &email);
    void addRefreshToken(const QString &refresh, const QString &email) { m_refresh.insert(refresh, email); }
    void revoke(const QString &refresh) { m_refresh.remove(refresh); }

    bool rotate = false;     // a new refresh token with every refresh, as Microsoft does
    int expiresIn = 3600;
    int refreshes = 0;
    int exchanges = 0;
    std::function<void(const QString &email, const QString &access)> issued;

protected:
    void incomingConnection(qintptr fd) override;

private:
    QByteArray handle(const QByteArray &body, int *status);

    struct Grant
    {
        QString challenge;
        QString email;
    };
    QHash<QString, Grant> m_codes;
    QHash<QString, QString> m_refresh; // refresh token -> email
    int m_next = 1;
};
