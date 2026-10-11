// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "MailWorker.h"
#include "mail/Types.h"

#include <QFuture>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <memory>
#include <optional>

namespace e3::sync {
class DeviceTransport;
class SyncSession;
} // namespace e3::sync

namespace e3 {

class BlobStore;
class Config;
class Database;
class OpLog;
namespace crypto {
class Pgp;
}

// One mailbox: its database, blobs and keys on disk under its own directory,
// a main-thread MailContext for the UI, and a worker thread for the network.
class Account : public QObject
{
    Q_OBJECT
public:
    struct Settings
    {
        QString addr;
        QString displayName;
        QString protocol = QStringLiteral("imap");
        QString inHost;
        quint16 inPort = 993;
        QString inSecurity = QStringLiteral("ssl");
        QString inUser;
        QString inPassword;
        QString smtpHost;
        quint16 smtpPort = 465;
        QString smtpSecurity = QStringLiteral("ssl");
        QString smtpUser;
        QString smtpPassword; // empty: same as incoming
        bool acceptInvalidCertificates = false;
        // Certificates trusted for these servers although TLS does not
        // trust them (ADR 0017); empty for none.
        QByteArray inCertSha256;
        QByteArray smtpCertSha256;
    };

    // What checking a server found: nothing (it works), an error, or a
    // certificate that needs a person's decision.
    struct CheckResult
    {
        QString error;
        QString server; // "in" or "smtp"
        std::optional<net::CertInfo> cert;
        bool ok() const { return error.isEmpty(); }
    };

    Account(int id, const QString &dir, QObject *parent = nullptr);
    ~Account() override;

    int id() const { return m_id; }
    QString dir() const { return m_dir; }
    QString addr() const;
    QString displayName() const;
    bool isConfigured() const;
    // Where the password ended up: "keyring" or "database".
    QString secretLocation() const;

    // Stores settings and secrets and creates the account's key. Does not
    // connect; see verify().
    void configure(const Settings &s);
    // In the OS keyring if there is one, otherwise in the database.
    void storePasswords(const QString &inPassword, const QString &smtpPassword);
    Settings settings() const;
    MailWorker::Credentials credentials() const;

    // Tries both servers with these settings on a pool thread. The future's
    // result is empty on success, or what went wrong.
    static QFuture<CheckResult> check(const Settings &s);

    void start();
    void stop();
    void syncNow();
    bool isRunning() const;
    QString status() const { return m_status; }
    // Older mail the first sync still has to download; 0 when done.
    int olderRemaining() const { return m_olderRemaining; }
    QString statusDetail() const { return m_statusDetail; }

    // The certificate the worker last refused, while status() is "cert":
    // which server, and its SHA-256. Trusting pins it and restarts.
    QString refusedServer() const { return m_refusedServer; }
    QByteArray refusedSha256() const { return m_refusedSha256; }
    void trustRefusedCertificate();
    // Back to TLS's own judgement for that server ("in" or "smtp").
    void forgetCertificate(const QString &server);
    void restart();

    // Other devices of this account (ADR 0010). The endpoint runs while the
    // account does and another device is paired, or while pairing.
    static bool devicesAvailable();
    // Starts the endpoint and the sync session if they are not running.
    // False when this build has no Iroh, or the endpoint cannot start.
    bool startDevices();
    void stopDevices();
    sync::DeviceTransport *deviceTransport() const { return m_transport.get(); }
    // Sends paired devices what they lack, soon.
    void pushSoon();
    bool isDeviceConnected(const QString &device) const;
    // Removes a paired device here and, through a device.unpair op, on the
    // others; it is refused from then on.
    void removeDevice(const QString &device);

    MailContext &ctx() { return *m_ctx; }
    // UI-side mutations call this so every view refreshes.
    void notifyChanged() { Q_EMIT mailChanged(); }

Q_SIGNALS:
    void mailChanged();
    void newMail(qint64 msgId);
    void statusChanged();

private:
    int m_id;
    QString m_dir;
    std::unique_ptr<Database> m_db;
    std::unique_ptr<BlobStore> m_blobs;
    std::unique_ptr<crypto::Pgp> m_pgp;
    std::unique_ptr<Config> m_config;
    std::unique_ptr<OpLog> m_ops;
    std::unique_ptr<MailContext> m_ctx;
    QPointer<MailWorker> m_worker;
    QString m_status = QStringLiteral("offline");
    int m_olderRemaining = 0;
    QString m_statusDetail;
    QString m_refusedServer;
    QByteArray m_refusedSha256;
    std::unique_ptr<sync::DeviceTransport> m_transport;
    std::unique_ptr<sync::SyncSession> m_sync;
    QTimer m_push; // gathers changes into one push
    QTimer m_dial; // dials paired devices that are not connected
};

} // namespace e3
