// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "engine/Autoconfig.h"
#include "net/OAuth.h"

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

// The add-a-mailbox form: suggests servers, then checks them before saving.
class SetupController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString addr MEMBER m_addr NOTIFY changed)
    Q_PROPERTY(QString password MEMBER m_password NOTIFY changed)
    Q_PROPERTY(QString displayName MEMBER m_name NOTIFY changed)
    Q_PROPERTY(QString protocol MEMBER m_protocol NOTIFY changed)
    Q_PROPERTY(QString inHost MEMBER m_inHost NOTIFY changed)
    Q_PROPERTY(int inPort MEMBER m_inPort NOTIFY changed)
    Q_PROPERTY(QString inSecurity MEMBER m_inSecurity NOTIFY changed)
    Q_PROPERTY(QString smtpHost MEMBER m_smtpHost NOTIFY changed)
    Q_PROPERTY(int smtpPort MEMBER m_smtpPort NOTIFY changed)
    Q_PROPERTY(QString smtpSecurity MEMBER m_smtpSecurity NOTIFY changed)
    Q_PROPERTY(bool showServers MEMBER m_showServers NOTIFY changed)
    // Where the server settings came from, and whether to check them.
    Q_PROPERTY(QString suggestionNote MEMBER m_note NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY busyChanged)
    // A certificate TLS does not trust, waiting for a decision: {server
    // ("in"/"smtp"), host, port, fingerprint, subject, issuer, expires,
    // problems, changed}; empty when there is none (ADR 0017).
    Q_PROPERTY(QVariantMap untrustedCertificate READ untrustedCertificate NOTIFY busyChanged)
    // OAuth 2 (ADR 0019): the provider that signs in for this address
    // ("Google", "Microsoft"), empty when there is none or this build has no
    // client ID for it; and whether the person has signed in.
    Q_PROPERTY(QString oauthProvider READ oauthProvider NOTIFY changed)
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY changed)

public:
    explicit SetupController(QObject *parent = nullptr);

    bool busy() const { return m_busy; }
    QString error() const { return m_error; }
    QVariantMap untrustedCertificate() const { return m_cert; }
    QString oauthProvider() const;
    bool signedIn() const { return !m_oauthRefresh.isEmpty() && m_oauthFor == m_addr.trimmed().toLower(); }

    // Opens the provider's sign-in in the browser; when it comes back, the
    // account is checked and added as submit() does.
    Q_INVOKABLE void signIn();
    // Fills the server fields from the address. Called when the address is
    // entered; the user can still edit everything.
    Q_INVOKABLE void suggest();
    Q_INVOKABLE void useProtocol(const QString &protocol);
    // Checks the servers, then creates and starts the account.
    Q_INVOKABLE void submit();
    // Trusts the certificate in untrustedCertificate for its server, and
    // checks again.
    Q_INVOKABLE void trustCertificate();

Q_SIGNALS:
    void changed();
    void busyChanged();
    void done(int accountId);

private:
    void setBusy(bool busy, const QString &error = {});
    void suggested(const e3::Autoconfig::Result &r);

    e3::Autoconfig *m_autoconfig;
    QString m_addr, m_password, m_name, m_protocol = QStringLiteral("imap");
    QString m_inHost, m_inUser, m_smtpUser, m_note;
    QString m_inSecurity = QStringLiteral("ssl"), m_smtpHost, m_smtpSecurity = QStringLiteral("ssl");
    int m_inPort = 993, m_smtpPort = 465;
    bool m_showServers = false;
    bool m_busy = false;
    QString m_error;
    QVariantMap m_cert;
    // Pins, each for the host and port it was given for.
    QByteArray m_inPin, m_smtpPin;
    QString m_inPinFor, m_smtpPinFor;
    QString m_suggestedFor;
    bool m_looking = false;     // a lookup is running
    bool m_submitAfter = false; // Connect was pressed during it
    QList<QVariantMap> m_incomingSuggestions;
    std::optional<e3::net::oauth::Provider> currentProvider() const;
    QString m_oauthIssuer; // from the last lookup
    QString m_oauthFor;    // the address signed in for
    QString m_oauthRefresh, m_oauthAccess;
    QDateTime m_oauthExpires;
    std::optional<e3::net::oauth::Provider> m_oauthProvider;
    QPointer<QObject> m_signIn;
};
