// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace e3 {
class Account;
}

// Settings, Backups, and setup's "Restore from a backup" (ADR 0020). The
// destination is described by a map: {type, path | url, user | endpoint,
// region, bucket, prefix, accessKey | host, port, path, keyFile}.
class BackupController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool isSetUp READ isSetUp NOTIFY changed)
    Q_PROPERTY(QString where READ where NOTIFY changed)
    Q_PROPERTY(QString last READ last NOTIFY changed)
    Q_PROPERTY(QString lastError READ lastError NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    // Something to confirm before going on: a server's certificate or SSH
    // host key, as {kind: "certificate" | "hostKey", fingerprint, changed}.
    Q_PROPERTY(QVariantMap confirm READ confirm NOTIFY changed)
    // Google Drive or OneDrive: signed in for storage.
    Q_PROPERTY(bool cloudSignedIn READ cloudSignedIn NOTIFY changed)
    Q_PROPERTY(QStringList types READ types CONSTANT)
    Q_PROPERTY(int percent READ percent NOTIFY changed)

public:
    explicit BackupController(QObject *parent = nullptr);

    bool isSetUp() const;
    QString where() const;
    QString last() const;
    QString lastError() const;
    bool busy() const;
    QString error() const { return m_error; }
    QVariantMap confirm() const { return m_confirm; }
    bool cloudSignedIn() const { return !m_cloudSecret.isEmpty(); }
    QStringList types() const;
    int percent() const { return m_percent; }

    // A recovery code to use as the passphrase: 25 letters and digits, in
    // groups of five, to print or write down.
    Q_INVOKABLE QString recoveryCode() const;
    // Opens Google's or Microsoft's sign-in for storage ("gdrive", "onedrive").
    Q_INVOKABLE void signInToCloud(const QString &type);
    Q_INVOKABLE void setUp(const QVariantMap &destination, const QString &secret, const QString &passphrase);
    // Trusts what `confirm` shows and tries the last action again.
    Q_INVOKABLE void trust();
    Q_INVOKABLE void backUpNow();
    Q_INVOKABLE void stop();
    // A new account from the newest backup there.
    Q_INVOKABLE void restore(const QVariantMap &destination, const QString &secret, const QString &passphrase);

Q_SIGNALS:
    void changed();
    void restored(int accountId);

private:
    e3::Account *account() const;
    QJsonObject withSignIn(const QVariantMap &destination) const;
    void setBusy(bool busy, const QString &error = {});
    void failed(const std::exception &e);

    bool m_busy = false;
    QString m_error;
    QVariantMap m_confirm;
    int m_percent = 0;
    // The last action, to repeat after trust().
    QVariantMap m_lastDestination;
    QString m_lastSecret, m_lastPassphrase;
    bool m_lastWasRestore = false;
    // Signed in for Google Drive or OneDrive: the refresh token and client.
    QString m_cloudSecret;
    QJsonObject m_cloudClient;
};
