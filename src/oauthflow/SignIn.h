// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "net/OAuth.h"

#include <QDateTime>
#include <QObject>
#include <QTimer>
#include <QUrl>

class QOAuth2AuthorizationCodeFlow;
class QOAuthHttpServerReplyHandler;

namespace e3 {

// Signing in with Google or Microsoft in the system's browser (ADR 0019):
// the authorization code flow with PKCE, the browser coming back to a
// one-time listener on 127.0.0.1. QtNetworkAuth does the protocol; the core
// renews the tokens afterwards (net::oauth::TokenSource).
class SignIn : public QObject
{
    Q_OBJECT
public:
    // `loginHint`: the address being added, so the provider offers it first.
    SignIn(const net::oauth::Provider &provider, const QString &loginHint, QObject *parent = nullptr);
    ~SignIn() override;

    // Emits openBrowser with the URL to show, or finished with an error.
    void start();

    // What the provider gave, once finished without an error.
    QString refreshToken() const { return m_refresh; }
    QString accessToken() const { return m_access; }
    QDateTime expires() const { return m_expires; }
    QString email() const { return m_email; } // from the ID token; may be empty
    const net::oauth::Provider &provider() const { return m_provider; }

Q_SIGNALS:
    void openBrowser(const QUrl &url);
    // Empty on success.
    void finished(const QString &error);

private:
    void finish(const QString &error);

    net::oauth::Provider m_provider;
    QString m_hint;
    QOAuth2AuthorizationCodeFlow *m_flow = nullptr;
    QOAuthHttpServerReplyHandler *m_handler = nullptr;
    QTimer m_timeout;
    bool m_done = false;
    QString m_refresh, m_access, m_email;
    QDateTime m_expires;
};

} // namespace e3
