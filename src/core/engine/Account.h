// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "MailWorker.h"
#include "mail/Types.h"

#include <QFuture>
#include <QObject>
#include <QPointer>

#include <memory>

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
    Settings settings() const;
    MailWorker::Credentials credentials() const;

    // Tries both servers with these settings on a pool thread. The future's
    // result is empty on success, or what went wrong.
    static QFuture<QString> check(const Settings &s);

    void start();
    void stop();
    void syncNow();
    bool isRunning() const;
    QString status() const { return m_status; }
    QString statusDetail() const { return m_statusDetail; }

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
    QString m_statusDetail;
};

} // namespace e3
