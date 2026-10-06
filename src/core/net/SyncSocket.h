// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QSslSocket>
#include <QString>

#include <functional>
#include <memory>
#include <stdexcept>

namespace e3::net {

class NetError : public std::runtime_error
{
public:
    explicit NetError(const QString &msg) : std::runtime_error(msg.toStdString()) {}
    QString message() const { return QString::fromStdString(what()); }
};

// A server rejected credentials. Distinct so the UI can ask again rather than
// retrying a wrong password until the account is locked.
class AuthError : public NetError
{
public:
    using NetError::NetError;
};

enum class Security { Ssl, StartTls, Plain };

Security securityFromString(const QString &s);
QString securityToString(Security s);

struct ServerSettings
{
    QString host;
    quint16 port = 0;
    Security security = Security::Ssl;
    QString user;
    QString password;
    // Only for tests and self-signed development servers; never set from the UI
    // without the user saying so.
    bool acceptInvalidCertificates = false;
};

// A socket whose reads block the calling thread by spinning a local event
// loop. This keeps protocol code linear and readable while staying on Qt's
// event-driven socket underneath, which, unlike QAbstractSocket's waitFor*
// calls, is reliable on Windows. Owned and used by exactly one thread.
class SyncSocket
{
public:
    explicit SyncSocket(int timeoutMs = 60000);
    ~SyncSocket();

    // For StartTls this connects in cleartext; call startTls() when the
    // protocol has negotiated it.
    void connectToHost(const ServerSettings &server);
    void startTls();
    bool isEncrypted() const;

    QByteArray readLine();            // without the line ending
    QByteArray read(qint64 bytes);
    void write(const QByteArray &data);
    void close();

    // Waits until a line is available, `stop()` returns true, or `maxMs`
    // passes. Returns true if a line is available. Used for IMAP IDLE and to
    // sleep between POP3 polls while staying wakeable.
    bool waitForLine(int maxMs, const std::function<bool()> &stop);

    void setTimeout(int ms) { m_timeoutMs = ms; }

private:
    void waitUntil(const std::function<bool()> &ready, const char *what);
    void configureTls(const ServerSettings &server);

    std::unique_ptr<QSslSocket> m_socket;
    QByteArray m_buffer;
    QString m_host;
    int m_timeoutMs;
    bool m_acceptInvalid = false;
};

} // namespace e3::net
