// SPDX-License-Identifier: MPL-2.0
#include "SyncSocket.h"

#include "util/Log.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QSslCertificate>
#include <QEventLoop>
#include <QTimer>

namespace e3::net {

Security securityFromString(const QString &s)
{
    if (s == QLatin1String("starttls"))
        return Security::StartTls;
    if (s == QLatin1String("plain"))
        return Security::Plain;
    return Security::Ssl;
}

QString securityToString(Security s)
{
    switch (s) {
    case Security::StartTls: return QStringLiteral("starttls");
    case Security::Plain: return QStringLiteral("plain");
    case Security::Ssl: break;
    }
    return QStringLiteral("ssl");
}

CertError::CertError(const CertInfo &i)
    : NetError(i.pinChanged ? QStringLiteral("%1: the server's certificate changed (now SHA-256 %2)")
                                  .arg(i.host, formatFingerprint(i.sha256))
                            : QStringLiteral("%1: the server's certificate is not trusted: %2 (SHA-256 %3)")
                                  .arg(i.host, i.problems.join(QStringLiteral("; ")), formatFingerprint(i.sha256))),
      info(i)
{
}

QString formatFingerprint(const QByteArray &sha256Hex)
{
    QStringList pairs;
    const QString hex = QString::fromLatin1(sha256Hex).toUpper();
    for (qsizetype i = 0; i < hex.size(); i += 2)
        pairs.append(hex.mid(i, 2));
    return pairs.join(u':');
}

QByteArray normalizeFingerprint(const QString &text)
{
    QByteArray out;
    for (const QChar c : text) {
        if (c.isDigit() || (c.toLower() >= u'a' && c.toLower() <= u'f'))
            out.append(char(c.toLower().unicode()));
    }
    return out.size() == 64 ? out : QByteArray();
}

SyncSocket::SyncSocket(int timeoutMs) : m_socket(std::make_unique<QSslSocket>()), m_timeoutMs(timeoutMs) {}

SyncSocket::~SyncSocket()
{
    m_socket->abort();
}

void SyncSocket::configureTls(const ServerSettings &server)
{
    m_acceptInvalid = server.acceptInvalidCertificates;
    m_pin = server.pinnedSha256;
    m_socket->setPeerVerifyName(server.host);
    if (m_acceptInvalid)
        m_socket->setPeerVerifyMode(QSslSocket::VerifyNone);
}

CertInfo SyncSocket::certInfo(const QSslCertificate &cert) const
{
    CertInfo i;
    i.host = m_host;
    i.port = m_port;
    i.sha256 = cert.digest(QCryptographicHash::Sha256).toHex();
    i.subject = cert.subjectDisplayName();
    i.issuer = cert.issuerDisplayName();
    i.expires = cert.expiryDate();
    for (const QSslError &e : m_sslErrors)
        i.problems.append(e.errorString());
    return i;
}

// Waits for TLS, then holds the certificate to the pin, if there is one.
void SyncSocket::handshake(const char *what)
{
    m_sslErrors.clear();
    try {
        waitUntil([this] { return m_socket->isEncrypted(); }, what);
    } catch (const NetError &) {
        if (!m_sslErrors.isEmpty() && !m_acceptInvalid) {
            const QSslCertificate cert = m_sslErrors.first().certificate().isNull()
                ? m_socket->peerCertificate()
                : m_sslErrors.first().certificate();
            CertInfo i = certInfo(cert);
            i.pinChanged = !m_pin.isEmpty();
            throw CertError(i);
        }
        throw;
    }
    if (!m_pin.isEmpty() && !m_acceptInvalid) {
        const QSslCertificate cert = m_socket->peerCertificate();
        if (cert.digest(QCryptographicHash::Sha256).toHex() != m_pin) {
            CertInfo i = certInfo(cert);
            i.pinChanged = true;
            m_socket->abort();
            throw CertError(i);
        }
    }
}

void SyncSocket::waitUntil(const std::function<bool()> &ready, const char *what)
{
    if (ready())
        return;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        loop.quit();
    });
    auto quit = [&] { loop.quit(); };
    QObject::connect(m_socket.get(), &QSslSocket::readyRead, &loop, quit);
    QObject::connect(m_socket.get(), &QSslSocket::connected, &loop, quit);
    QObject::connect(m_socket.get(), &QSslSocket::encrypted, &loop, quit);
    QObject::connect(m_socket.get(), &QSslSocket::disconnected, &loop, quit);
    QObject::connect(m_socket.get(), &QSslSocket::errorOccurred, &loop, quit);
    QObject::connect(m_socket.get(), &QSslSocket::bytesWritten, &loop, quit);
    deadline.start(m_timeoutMs);
    // Wake regularly to look at `abort`.
    QTimer tick;
    if (m_abort) {
        QObject::connect(&tick, &QTimer::timeout, &loop, quit);
        tick.start(250);
    }
    while (true) {
        m_buffer += m_socket->readAll();
        if (ready())
            return;
        if (m_abort && m_abort())
            throw NetError(QStringLiteral("%1: stopped").arg(m_host));
        if (timedOut)
            throw NetError(QStringLiteral("%1: timed out waiting for %2").arg(m_host, QLatin1String(what)));
        if (m_socket->state() == QAbstractSocket::UnconnectedState) {
            const QString err = m_socket->error() == QAbstractSocket::UnknownSocketError
                ? QStringLiteral("connection closed")
                : m_socket->errorString();
            throw NetError(QStringLiteral("%1: %2").arg(m_host, err));
        }
        loop.exec();
    }
}

void SyncSocket::connectToHost(const ServerSettings &server)
{
    m_host = server.host;
    m_port = server.port;
    m_abort = server.abort;
    m_buffer.clear();
    configureTls(server);
    QObject::disconnect(m_socket.get(), &QSslSocket::sslErrors, nullptr, nullptr);
    QObject::connect(m_socket.get(), &QSslSocket::sslErrors, m_socket.get(), [this](const QList<QSslError> &errors) {
        m_sslErrors = errors;
        if (m_acceptInvalid) {
            m_socket->ignoreSslErrors();
            return;
        }
        // The pinned certificate is trusted whatever TLS says of it, and only
        // that certificate.
        const QSslCertificate cert = m_socket->peerCertificate();
        if (!m_pin.isEmpty() && !cert.isNull() && cert.digest(QCryptographicHash::Sha256).toHex() == m_pin)
            m_socket->ignoreSslErrors(errors);
    });
    qCDebug(lcNet) << "connecting" << server.host << server.port << securityToString(server.security);
    if (server.security == Security::Ssl) {
        m_socket->connectToHostEncrypted(server.host, server.port);
        handshake("TLS handshake");
    } else {
        m_socket->connectToHost(server.host, server.port);
        waitUntil([this] { return m_socket->state() == QAbstractSocket::ConnectedState; }, "connection");
    }
}

void SyncSocket::startTls()
{
    m_buffer.clear();
    m_socket->startClientEncryption();
    handshake("STARTTLS handshake");
}

bool SyncSocket::isEncrypted() const
{
    return m_socket->isEncrypted();
}

QByteArray SyncSocket::readLine()
{
    waitUntil([this] { return m_buffer.contains('\n'); }, "server response");
    const qsizetype nl = m_buffer.indexOf('\n');
    QByteArray line = m_buffer.left(nl);
    m_buffer.remove(0, nl + 1);
    if (line.endsWith('\r'))
        line.chop(1);
    return line;
}

QByteArray SyncSocket::read(qint64 bytes)
{
    waitUntil([&] { return m_buffer.size() >= bytes; }, "data");
    QByteArray out = m_buffer.left(bytes);
    m_buffer.remove(0, bytes);
    return out;
}

void SyncSocket::write(const QByteArray &data)
{
    if (m_socket->write(data) != data.size())
        throw NetError(QStringLiteral("%1: write failed: %2").arg(m_host, m_socket->errorString()));
    waitUntil([this] { return m_socket->bytesToWrite() == 0; }, "write");
}

void SyncSocket::close()
{
    m_socket->disconnectFromHost();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
}

bool SyncSocket::waitForLine(int maxMs, const std::function<bool()> &stop)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!m_buffer.contains('\n')) {
        if (stop && stop())
            return false;
        if (elapsed.elapsed() >= maxMs)
            return false;
        // Wake regularly to look at `stop`, without busy-waiting.
        QEventLoop loop;
        QTimer tick;
        tick.setSingleShot(true);
        QObject::connect(&tick, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(m_socket.get(), &QSslSocket::readyRead, &loop, &QEventLoop::quit);
        QObject::connect(m_socket.get(), &QSslSocket::disconnected, &loop, &QEventLoop::quit);
        tick.start(qMin<qint64>(250, qMax<qint64>(1, maxMs - elapsed.elapsed())));
        loop.exec();
        m_buffer += m_socket->readAll();
        if (m_socket->state() == QAbstractSocket::UnconnectedState && !m_buffer.contains('\n'))
            throw NetError(QStringLiteral("%1: connection closed").arg(m_host));
    }
    return true;
}

} // namespace e3::net
