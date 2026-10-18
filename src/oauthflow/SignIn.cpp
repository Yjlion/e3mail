// SPDX-License-Identifier: MPL-2.0
#include "SignIn.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QOAuth2AuthorizationCodeFlow>
#include <QOAuthHttpServerReplyHandler>

namespace e3 {

SignIn::SignIn(const net::oauth::Provider &provider, const QString &loginHint, QObject *parent)
    : QObject(parent), m_provider(provider), m_hint(loginHint)
{
    // A sign-in left open in the browser does not wait forever.
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(10 * 60 * 1000);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        finish(QCoreApplication::translate("SignIn", "The sign-in took too long. Try again."));
    });
}

SignIn::~SignIn() = default;

void SignIn::start()
{
    m_handler = new QOAuthHttpServerReplyHandler(QHostAddress::LocalHost, 0, this);
    if (!m_handler->isListening()) {
        finish(QCoreApplication::translate("SignIn", "Cannot listen for the browser's answer on this device."));
        return;
    }
    m_handler->setCallbackText(
        QCoreApplication::translate("SignIn", "Signed in. You can close this page and go back to e3mail."));
    m_flow = new QOAuth2AuthorizationCodeFlow(this);
    m_flow->setReplyHandler(m_handler);
    m_flow->setAuthorizationUrl(m_provider.authUrl);
    m_flow->setTokenUrl(m_provider.tokenUrl);
    m_flow->setClientIdentifier(m_provider.clientId);
    if (!m_provider.clientSecret.isEmpty())
        m_flow->setClientIdentifierSharedKey(m_provider.clientSecret);
    QSet<QByteArray> scopes;
    for (const QString &s : std::as_const(m_provider.scopes))
        scopes.insert(s.toUtf8());
    m_flow->setRequestedScopeTokens(scopes);
    // PKCE (S256) is QtNetworkAuth's default.
    m_flow->setModifyParametersFunction([this](QAbstractOAuth::Stage stage, QMultiMap<QString, QVariant> *params) {
        if (stage != QAbstractOAuth::Stage::RequestingAuthorization)
            return;
        for (auto it = m_provider.authParams.cbegin(); it != m_provider.authParams.cend(); ++it)
            params->replace(it.key(), it.value());
        if (!m_hint.isEmpty())
            params->replace(QStringLiteral("login_hint"), m_hint);
    });
    connect(m_flow, &QAbstractOAuth::authorizeWithBrowser, this, &SignIn::openBrowser);
    connect(m_flow, &QAbstractOAuth::granted, this, [this] {
        m_access = m_flow->token();
        m_refresh = m_flow->refreshToken();
        m_expires = m_flow->expirationAt();
        m_email = net::oauth::emailFromIdToken(m_flow->idToken());
        if (m_refresh.isEmpty())
            finish(QCoreApplication::translate("SignIn", "%1 did not allow e3mail to stay signed in.").arg(m_provider.name));
        else
            finish(QString());
    });
    connect(m_flow, &QAbstractOAuth::requestFailed, this, [this](QAbstractOAuth::Error) {
        finish(QCoreApplication::translate("SignIn", "%1 refused the sign-in.").arg(m_provider.name));
    });
    m_timeout.start();
    m_flow->grant();
}

void SignIn::finish(const QString &error)
{
    if (m_done)
        return;
    m_done = true;
    m_timeout.stop();
    if (m_handler)
        m_handler->close();
    Q_EMIT finished(error);
}

} // namespace e3
