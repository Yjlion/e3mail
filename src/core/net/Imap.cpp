// SPDX-License-Identifier: MPL-2.0
#include "Imap.h"

#include "util/Log.h"

#include <QElapsedTimer>
#include <QRegularExpression>

namespace e3::net {

namespace {

// Returns the byte count of a trailing "{123}" literal marker, or -1.
qint64 trailingLiteral(const QByteArray &line)
{
    if (!line.endsWith('}'))
        return -1;
    const qsizetype open = line.lastIndexOf('{');
    if (open < 0)
        return -1;
    QByteArray n = line.mid(open + 1, line.size() - open - 2);
    if (n.endsWith('+'))
        n.chop(1);
    bool ok = false;
    const qint64 v = n.toLongLong(&ok);
    return ok ? v : -1;
}

quint32 numberAfter(const QByteArray &line, const QByteArray &key)
{
    const qsizetype i = line.toUpper().indexOf(key);
    if (i < 0)
        return 0;
    qsizetype j = i + key.size();
    while (j < line.size() && line[j] == ' ')
        ++j;
    qsizetype k = j;
    while (k < line.size() && std::isdigit(static_cast<unsigned char>(line[k])))
        ++k;
    return line.mid(j, k - j).toUInt();
}

} // namespace

QByteArray ImapClient::quote(const QByteArray &s)
{
    for (char c : s) {
        if (c == '\r' || c == '\n' || c == '\0' || static_cast<unsigned char>(c) >= 0x80)
            return {};
    }
    QByteArray q = s;
    q.replace('\\', "\\\\").replace('"', "\\\"");
    return '"' + q + '"';
}

QByteArray ImapClient::nextTag()
{
    return "e" + QByteArray::number(++m_tag);
}

void ImapClient::throwIfFailed(const Response &r, const char *what)
{
    if (r.status != "OK")
        throw NetError(QStringLiteral("IMAP %1 failed: %2 %3")
                           .arg(QLatin1String(what), QString::fromUtf8(r.status), QString::fromUtf8(r.text)));
}

void ImapClient::readUntagged(Response &r, QByteArray line)
{
    QList<QByteArray> literals;
    qint64 n;
    while ((n = trailingLiteral(line)) >= 0) {
        literals.append(m_socket.read(n));
        line += "\x01"; // marks where the literal sat
        line += m_socket.readLine();
    }
    if (line.toUpper().contains("[CAPABILITY ") || line.toUpper().startsWith("* CAPABILITY "))
        parseCapabilities(line);
    r.untagged.append(line);
    r.literals.append(literals);
}

ImapClient::Response ImapClient::readUntilTagged(const QByteArray &tag)
{
    Response r;
    while (true) {
        QByteArray line = m_socket.readLine();
        if (line.startsWith(tag + ' ')) {
            const QByteArray rest = line.mid(tag.size() + 1);
            const qsizetype sp = rest.indexOf(' ');
            r.status = (sp < 0 ? rest : rest.left(sp)).toUpper();
            r.text = sp < 0 ? QByteArray() : rest.mid(sp + 1);
            if (r.text.toUpper().contains("[CAPABILITY "))
                parseCapabilities(r.text);
            return r;
        }
        if (line.startsWith("* ")) {
            readUntagged(r, line);
        } else if (line.startsWith('+')) {
            r.untagged.append(line); // continuation request, handled by callers
            r.literals.append(QList<QByteArray>());
            return r;
        }
    }
}

ImapClient::Response ImapClient::command(const QByteArray &cmd)
{
    const QByteArray tag = nextTag();
    m_socket.write(tag + ' ' + cmd + "\r\n");
    return readUntilTagged(tag);
}

void ImapClient::parseCapabilities(const QByteArray &line)
{
    QByteArray s = line.toUpper();
    const qsizetype i = s.indexOf("CAPABILITY ");
    s = s.mid(i + 11);
    if (const qsizetype end = s.indexOf(']'); end >= 0)
        s.truncate(end);
    m_caps.clear();
    for (const QByteArray &c : s.split(' ')) {
        if (!c.isEmpty())
            m_caps.insert(c);
    }
}

void ImapClient::connect(const ServerSettings &server)
{
    m_socket.connectToHost(server);
    const QByteArray greeting = m_socket.readLine();
    if (!greeting.startsWith("* OK") && !greeting.startsWith("* PREAUTH"))
        throw NetError(QStringLiteral("IMAP: unexpected greeting: %1").arg(QString::fromUtf8(greeting)));
    if (greeting.toUpper().contains("[CAPABILITY "))
        parseCapabilities(greeting);
    if (m_caps.isEmpty())
        throwIfFailed(command("CAPABILITY"), "CAPABILITY");

    if (server.security == Security::StartTls) {
        if (!hasCapability("STARTTLS"))
            throw NetError(QStringLiteral("IMAP: %1 does not offer STARTTLS").arg(server.host));
        throwIfFailed(command("STARTTLS"), "STARTTLS");
        m_socket.startTls();
        throwIfFailed(command("CAPABILITY"), "CAPABILITY");
    }
    if (greeting.startsWith("* PREAUTH"))
        return;

    const QByteArray user = server.user.toUtf8();
    const QByteArray pass = server.password.toUtf8();
    Response r;
    if (hasCapability("AUTH=PLAIN")) {
        const QByteArray tag = nextTag();
        const QByteArray initial = (QByteArray(1, '\0') + user + '\0' + pass).toBase64();
        if (hasCapability("SASL-IR")) {
            m_socket.write(tag + " AUTHENTICATE PLAIN " + initial + "\r\n");
            r = readUntilTagged(tag);
        } else {
            m_socket.write(tag + " AUTHENTICATE PLAIN\r\n");
            r = readUntilTagged(tag);
            if (!r.untagged.isEmpty() && r.untagged.last().startsWith('+')) {
                m_socket.write(initial + "\r\n");
                r = readUntilTagged(tag);
            }
        }
    } else {
        const QByteArray qu = quote(user);
        const QByteArray qp = quote(pass);
        if (qu.isEmpty() || qp.isEmpty())
            throw NetError(QStringLiteral("IMAP: server offers no AUTH=PLAIN and the credentials need it"));
        if (hasCapability("LOGINDISABLED"))
            throw NetError(QStringLiteral("IMAP: login is disabled on this connection"));
        r = command("LOGIN " + qu + ' ' + qp);
    }
    if (r.status == "NO")
        throw AuthError(QStringLiteral("IMAP login failed: %1").arg(QString::fromUtf8(r.text)));
    throwIfFailed(r, "login");
    if (!r.text.toUpper().contains("[CAPABILITY "))
        throwIfFailed(command("CAPABILITY"), "CAPABILITY");
}

ImapClient::Mailbox ImapClient::select(const QString &mailbox)
{
    const Response r = command("SELECT " + quote(mailbox.toUtf8()));
    throwIfFailed(r, "SELECT");
    Mailbox mb;
    for (const QByteArray &line : r.untagged) {
        const QByteArray u = line.toUpper();
        if (u.contains("[UIDVALIDITY "))
            mb.uidValidity = numberAfter(line, "[UIDVALIDITY ");
        else if (u.contains("[UIDNEXT "))
            mb.uidNext = numberAfter(line, "[UIDNEXT ");
        else if (u.endsWith(" EXISTS"))
            mb.exists = line.mid(2, line.indexOf(' ', 2) - 2).toUInt();
    }
    return mb;
}

QList<quint32> ImapClient::uids()
{
    const Response r = command("UID SEARCH ALL");
    throwIfFailed(r, "UID SEARCH");
    QList<quint32> out;
    for (const QByteArray &line : r.untagged) {
        if (!line.toUpper().startsWith("* SEARCH"))
            continue;
        for (const QByteArray &t : line.mid(8).split(' ')) {
            bool ok = false;
            const quint32 v = t.toUInt(&ok);
            if (ok)
                out.append(v);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

QByteArray ImapClient::fetch(quint32 uid)
{
    const Response r = command("UID FETCH " + QByteArray::number(uid) + " (UID BODY.PEEK[])");
    throwIfFailed(r, "UID FETCH");
    for (qsizetype i = 0; i < r.untagged.size(); ++i) {
        const QByteArray u = r.untagged[i].toUpper();
        if (!u.contains(" FETCH ") || r.literals[i].isEmpty())
            continue;
        // Servers may send unsolicited FETCH responses for other messages.
        if (numberAfter(r.untagged[i], "UID ") != uid && u.contains("UID "))
            continue;
        return r.literals[i].first();
    }
    throw NetError(QStringLiteral("IMAP: message UID %1 is gone").arg(uid));
}

void ImapClient::remove(const QList<quint32> &uids)
{
    if (uids.isEmpty())
        return;
    QByteArray set;
    for (quint32 u : uids) {
        if (!set.isEmpty())
            set += ',';
        set += QByteArray::number(u);
    }
    throwIfFailed(command("UID STORE " + set + " +FLAGS.SILENT (\\Deleted)"), "UID STORE");
    // UID EXPUNGE removes only what we marked; plain EXPUNGE would also remove
    // anything another client had marked, which is not ours to decide.
    if (hasCapability("UIDPLUS"))
        throwIfFailed(command("UID EXPUNGE " + set), "UID EXPUNGE");
    else
        throwIfFailed(command("EXPUNGE"), "EXPUNGE");
}

bool ImapClient::idle(int maxMs, const std::function<bool()> &stop)
{
    if (!hasCapability("IDLE")) {
        // Sleep in small steps and ask with NOOP.
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < maxMs) {
            if (stop && stop())
                return false;
            const Response r = command("NOOP");
            for (const QByteArray &line : r.untagged) {
                if (line.toUpper().endsWith(" EXISTS") || line.toUpper().endsWith(" EXPUNGE"))
                    return true;
            }
            m_socket.waitForLine(qMin<qint64>(30000, maxMs - t.elapsed()), stop);
        }
        return false;
    }
    const QByteArray tag = nextTag();
    m_socket.write(tag + " IDLE\r\n");
    const QByteArray cont = m_socket.readLine();
    if (!cont.startsWith('+'))
        throw NetError(QStringLiteral("IMAP IDLE refused: %1").arg(QString::fromUtf8(cont)));
    bool changed = false;
    QElapsedTimer t;
    t.start();
    while (!changed && t.elapsed() < maxMs) {
        if (!m_socket.waitForLine(int(maxMs - t.elapsed()), stop))
            break;
        const QByteArray line = m_socket.readLine().toUpper();
        if (line.endsWith(" EXISTS") || line.endsWith(" EXPUNGE") || line.contains(" RECENT"))
            changed = true;
    }
    m_socket.write("DONE\r\n");
    throwIfFailed(readUntilTagged(tag), "IDLE");
    return changed;
}

void ImapClient::logout()
{
    try {
        command("LOGOUT");
    } catch (const NetError &) {
    }
    m_socket.close();
}

} // namespace e3::net
