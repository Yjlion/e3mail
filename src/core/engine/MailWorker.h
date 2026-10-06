// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "net/SyncSocket.h"

#include <QThread>

#include <atomic>

namespace e3 {

struct MailContext;

// One background thread per account. It owns its own database connection and
// keyring, so nothing it does blocks the UI: it sends the outbox, fetches new
// mail (IMAP IDLE, or POP3 polling), applies server retention, and runs
// housekeeping. It talks to the UI thread only through queued signals.
class MailWorker : public QThread
{
    Q_OBJECT
public:
    struct Credentials
    {
        net::ServerSettings incoming;
        net::ServerSettings smtp;
        QString protocol; // imap | pop3
    };

    MailWorker(const QString &accountDir, const Credentials &creds, QObject *parent = nullptr);
    ~MailWorker() override;

    // Interrupts a wait (IDLE or poll sleep) to send and fetch now.
    void wake() { m_wake = true; }
    void requestStop() { m_stop = true; }

    // One synchronous pass: send, fetch, retention. For the CLI and tests.
    static void runOnce(const QString &accountDir, const Credentials &creds);

Q_SIGNALS:
    void mailChanged();
    void newMail(qint64 msgId);
    void status(const QString &state, const QString &detail); // online | offline | error | auth
    void sent(qint64 msgId);

protected:
    void run() override;

private:
    class Session;
    bool shouldStop() const { return m_stop; }
    // Sleeps up to `ms`, returning early on wake or stop.
    void nap(int ms);

    QString m_dir;
    Credentials m_creds;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_wake{false};
};

} // namespace e3
