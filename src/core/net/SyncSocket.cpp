// SPDX-License-Identifier: MPL-2.0
#include "SyncSocket.h"

#include "util/Log.h"

#include <QElapsedTimer>
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

SyncSocket::SyncSocket(int timeoutMs) : m_socket(std::make_unique<QSslSocket>()), m_timeoutMs(timeoutMs) {}

SyncSocket::~SyncSocket()
{
    m_socket->abort();
}

void SyncSocket::configureTls(const ServerSettings &server)
{
    m_acceptInvalid = server.acceptInvalidCertificates;
    m_socket->setPeerVerifyName(server.host);
    if (m_acceptInvalid)
        m_socket->setPeerVerifyMode(QSslSocket::VerifyNone);
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
    while (true) {
        m_buffer += m_socket->readAll();
        if (ready())
            return;
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
    m_buffer.clear();
    configureTls(server);
    if (m_acceptInvalid) {
        QObject::connect(m_socket.get(), &QSslSocket::sslErrors, m_socket.get(),
                         [s = m_socket.get()](const QList<QSslError> &) { s->ignoreSslErrors(); });
    }
    qCDebug(lcNet) << "connecting" << server.host << server.port << securityToString(server.security);
    if (server.security == Security::Ssl) {
        m_socket->connectToHostEncrypted(server.host, server.port);
        waitUntil([this] { return m_socket->isEncrypted(); }, "TLS handshake");
    } else {
        m_socket->connectToHost(server.host, server.port);
        waitUntil([this] { return m_socket->state() == QAbstractSocket::ConnectedState; }, "connection");
    }
}

void SyncSocket::startTls()
{
    m_buffer.clear();
    m_socket->startClientEncryption();
    waitUntil([this] { return m_socket->isEncrypted(); }, "STARTTLS handshake");
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
