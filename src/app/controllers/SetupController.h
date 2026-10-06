// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QObject>
#include <QtQml/qqmlregistration.h>

namespace e3 {
class Autoconfig;
}

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
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY busyChanged)

public:
    explicit SetupController(QObject *parent = nullptr);

    bool busy() const { return m_busy; }
    QString error() const { return m_error; }

    // Fills the server fields from the address. Called when the address is
    // entered; the user can still edit everything.
    Q_INVOKABLE void suggest();
    Q_INVOKABLE void useProtocol(const QString &protocol);
    // Checks the servers, then creates and starts the account.
    Q_INVOKABLE void submit();

Q_SIGNALS:
    void changed();
    void busyChanged();
    void done(int accountId);

private:
    void setBusy(bool busy, const QString &error = {});

    e3::Autoconfig *m_autoconfig;
    QString m_addr, m_password, m_name, m_protocol = QStringLiteral("imap");
    QString m_inHost, m_inSecurity = QStringLiteral("ssl"), m_smtpHost, m_smtpSecurity = QStringLiteral("ssl");
    int m_inPort = 993, m_smtpPort = 465;
    bool m_showServers = false;
    bool m_busy = false;
    QString m_error;
    QString m_suggestedFor;
    QList<QVariantMap> m_incomingSuggestions;
};
