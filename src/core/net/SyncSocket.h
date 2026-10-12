// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QSslSocket>
#include <QString>
#include <QStringList>

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

// A server's certificate, as shown before someone decides to trust it.
struct CertInfo
{
    QString host;
    quint16 port = 0;
    QByteArray sha256;     // lower-case hex of the DER encoding: what a pin holds
    QString subject;
    QString issuer;
    QDateTime expires;
    QStringList problems;  // why TLS did not trust it; empty when the pin changed
    bool pinChanged = false;
};

// TLS refused the server's certificate, or it is not the pinned one. Carries
// the certificate so setup can show it and ask (ADR 0017).
class CertError : public NetError
{
public:
    explicit CertError(const CertInfo &info);
    CertInfo info;
};

// "AB:CD:…" for people; any spacing or colons back to the stored form.
QString formatFingerprint(const QByteArray &sha256Hex);
QByteArray normalizeFingerprint(const QString &text);

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
    // The one certificate trusted for this server although TLS does not trust
    // it (self-signed, or a name that does not match), and the only one
    // accepted from it: a different certificate is refused (ADR 0017).
    QByteArray pinnedSha256;
    // Polled while waiting on the server; true gives up at once. Set by the
    // worker, so stopping an account never waits for a slow or stalled
    // server (trap 38).
    std::function<bool()> abort;
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
    void handshake(const char *what);
    CertInfo certInfo(const QSslCertificate &cert) const;

    std::unique_ptr<QSslSocket> m_socket;
    QByteArray m_buffer;
    QString m_host;
    int m_timeoutMs;
    quint16 m_port = 0;
    bool m_acceptInvalid = false;
    QByteArray m_pin;
    QList<QSslError> m_sslErrors; // from the last handshake
    std::function<bool()> m_abort;
};

} // namespace e3::net
