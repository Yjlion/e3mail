// SPDX-License-Identifier: MPL-2.0
#include "Smtp.h"

#include "util/Log.h"

#include <QHostInfo>

namespace e3::net {

QString SmtpClient::Reply::text() const
{
    QStringList parts;
    for (const QByteArray &l : lines)
        parts.append(QString::fromUtf8(l));
    return QString::number(code) + u' ' + parts.join(u' ');
}

SmtpClient::Reply SmtpClient::readReply()
{
    Reply r;
    while (true) {
        const QByteArray line = m_socket.readLine();
        if (line.size() < 3)
            throw NetError(QStringLiteral("SMTP: malformed reply: %1").arg(QString::fromUtf8(line)));
        r.code = line.left(3).toInt();
        r.lines.append(line.mid(4));
        if (line.size() < 4 || line[3] != '-')
            return r;
    }
}

SmtpClient::Reply SmtpClient::command(const QByteArray &line)
{
    m_socket.write(line + "\r\n");
    return readReply();
}

void SmtpClient::expect(const Reply &r, int lo, int hi, const char *what)
{
    if (r.code < lo || r.code > hi)
        throw NetError(QStringLiteral("SMTP %1 failed: %2").arg(QLatin1String(what), r.text()));
}

void SmtpClient::ehlo()
{
    QByteArray name = QHostInfo::localHostName().toUtf8();
    if (name.isEmpty() || !name.contains('.'))
        name = "[127.0.0.1]"; // a non-FQDN EHLO is rejected by some servers
    Reply r = command("EHLO " + name);
    if (r.code != 250) {
        r = command("HELO " + name);
        expect(r, 250, 250, "HELO");
    }
    m_caps.clear();
    m_authMechs.clear();
    for (qsizetype i = 1; i < r.lines.size(); ++i) {
        const QByteArray l = r.lines[i].toUpper();
        m_caps.insert(l.split(' ').first());
        if (l.startsWith("AUTH ") || l.startsWith("AUTH="))
            m_authMechs += l.mid(5).split(' ');
    }
}

void SmtpClient::connect(const ServerSettings &server)
{
    m_socket.connectToHost(server);
    expect(readReply(), 220, 220, "greeting");
    ehlo();
    if (server.security == Security::StartTls) {
        if (!m_caps.contains("STARTTLS"))
            throw NetError(QStringLiteral("SMTP: %1 does not offer STARTTLS").arg(server.host));
        expect(command("STARTTLS"), 220, 220, "STARTTLS");
        m_socket.startTls();
        ehlo();
    }
    if (server.user.isEmpty())
        return;
    const QByteArray user = server.user.toUtf8();
    const QByteArray pass = server.password.toUtf8();
    Reply r;
    if (m_authMechs.contains("PLAIN") || m_authMechs.isEmpty()) {
        r = command("AUTH PLAIN " + (QByteArray(1, '\0') + user + '\0' + pass).toBase64());
    } else if (m_authMechs.contains("LOGIN")) {
        r = command("AUTH LOGIN");
        if (r.code == 334)
            r = command(user.toBase64());
        if (r.code == 334)
            r = command(pass.toBase64());
    } else {
        throw NetError(QStringLiteral("SMTP: no supported AUTH mechanism"));
    }
    if (r.code == 535 || r.code == 534)
        throw AuthError(QStringLiteral("SMTP login failed: %1").arg(r.text()));
    expect(r, 235, 235, "AUTH");
}

QByteArray SmtpClient::prepareData(const QByteArray &message)
{
    QByteArray out;
    out.reserve(message.size() + message.size() / 50 + 8);
    const QList<QByteArray> lines = message.split('\n');
    for (qsizetype i = 0; i < lines.size(); ++i) {
        QByteArray line = lines[i];
        if (i == lines.size() - 1 && line.isEmpty())
            break; // trailing newline
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.startsWith('.'))
            out.append('.');
        out.append(line);
        out.append("\r\n");
    }
    out.append(".\r\n");
    return out;
}

QStringList SmtpClient::send(const QString &from, const QStringList &recipients, const QByteArray &message)
{
    const bool smtputf8 = m_caps.contains("SMTPUTF8");
    QByteArray mailFrom = "MAIL FROM:<" + from.toUtf8() + ">";
    if (m_caps.contains("8BITMIME"))
        mailFrom += " BODY=8BITMIME";
    const QByteArray envelope = (from + recipients.join(u',')).toUtf8();
    if (smtputf8 && envelope.size() != from.size() + recipients.join(u',').size())
        mailFrom += " SMTPUTF8"; // an internationalised address is present
    expect(command(mailFrom), 250, 250, "MAIL FROM");

    QStringList refused;
    for (const QString &rcpt : recipients) {
        const Reply r = command("RCPT TO:<" + rcpt.toUtf8() + ">");
        if (r.code != 250 && r.code != 251) {
            qCWarning(lcNet) << "recipient refused" << rcpt << r.text();
            refused.append(rcpt);
        }
    }
    if (refused.size() == recipients.size()) {
        command("RSET");
        throw NetError(QStringLiteral("SMTP: every recipient was refused"));
    }
    expect(command("DATA"), 354, 354, "DATA");
    m_socket.write(prepareData(message));
    expect(readReply(), 250, 250, "message");
    return refused;
}

void SmtpClient::quit()
{
    try {
        command("QUIT");
    } catch (const NetError &) {
    }
    m_socket.close();
}

} // namespace e3::net
