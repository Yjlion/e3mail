// SPDX-License-Identifier: MPL-2.0
#include "Pop3.h"

namespace e3::net {

QByteArray Pop3Client::command(const QByteArray &line, const char *what)
{
    m_socket.write(line + "\r\n");
    const QByteArray reply = m_socket.readLine();
    if (!reply.startsWith("+OK"))
        throw NetError(QStringLiteral("POP3 %1 failed: %2").arg(QLatin1String(what), QString::fromUtf8(reply)));
    return reply;
}

QByteArray Pop3Client::readMultiline()
{
    QByteArray out;
    while (true) {
        QByteArray line = m_socket.readLine();
        if (line == ".")
            return out;
        if (line.startsWith('.'))
            line.remove(0, 1); // dot-unstuffing
        out += line;
        out += "\r\n";
    }
}

void Pop3Client::connect(const ServerSettings &server)
{
    m_socket.connectToHost(server);
    const QByteArray greeting = m_socket.readLine();
    if (!greeting.startsWith("+OK"))
        throw NetError(QStringLiteral("POP3: unexpected greeting: %1").arg(QString::fromUtf8(greeting)));

    m_socket.write("CAPA\r\n");
    if (m_socket.readLine().startsWith("+OK")) {
        const QByteArray caps = readMultiline();
        for (const QByteArray &c : caps.split('\n'))
            m_caps.append(c.trimmed().toUpper());
    }
    if (server.security == Security::StartTls) {
        command("STLS", "STLS");
        m_socket.startTls();
    }
    const QByteArray user = server.user.toUtf8();
    const QByteArray pass = server.password.toUtf8();
    bool saslPlain = false;
    for (const QByteArray &c : std::as_const(m_caps)) {
        if (c.startsWith("SASL") && c.contains("PLAIN"))
            saslPlain = true;
    }
    QByteArray reply;
    if (saslPlain) {
        m_socket.write("AUTH PLAIN " + (QByteArray(1, '\0') + user + '\0' + pass).toBase64() + "\r\n");
        reply = m_socket.readLine();
    } else {
        m_socket.write("USER " + user + "\r\n");
        reply = m_socket.readLine();
        if (reply.startsWith("+OK")) {
            m_socket.write("PASS " + pass + "\r\n");
            reply = m_socket.readLine();
        }
    }
    if (!reply.startsWith("+OK"))
        throw AuthError(QStringLiteral("POP3 login failed: %1").arg(QString::fromUtf8(reply)));
}

QList<Pop3Client::Entry> Pop3Client::list()
{
    command("UIDL", "UIDL");
    QList<Entry> out;
    for (const QByteArray &line : readMultiline().split('\n')) {
        const QList<QByteArray> parts = line.trimmed().split(' ');
        if (parts.size() < 2)
            continue;
        bool ok = false;
        const int n = parts[0].toInt(&ok);
        if (ok)
            out.append({n, QString::fromLatin1(parts[1])});
    }
    return out;
}

QByteArray Pop3Client::retrieve(int number)
{
    command("RETR " + QByteArray::number(number), "RETR");
    return readMultiline();
}

void Pop3Client::remove(int number)
{
    command("DELE " + QByteArray::number(number), "DELE");
}

void Pop3Client::quit()
{
    // Deletions only take effect if QUIT succeeds; a dropped connection rolls
    // them back, which is the safe direction.
    try {
        command("QUIT", "QUIT");
    } catch (const NetError &) {
        m_socket.close();
        throw;
    }
    m_socket.close();
}

} // namespace e3::net
