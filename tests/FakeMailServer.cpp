// SPDX-License-Identifier: MPL-2.0
#include "FakeMailServer.h"

#include <QHostAddress>
#include <QTcpSocket>

#include <memory>

struct FakeMailServer::ImapConn
{
    QString user;
    QByteArray pendingAuthTag;
    QByteArray idleTag;
};
struct FakeMailServer::Pop3Conn
{
    QString user;
    QString pendingUser;
    QList<int> dele;
};
struct FakeMailServer::SmtpConn
{
    bool authed = false;
    bool inData = false;
    QByteArray data;
    QString from;
    QStringList rcpts;
};

namespace {

QByteArray unquote(QByteArray s)
{
    s = s.trimmed();
    if (s.startsWith('"') && s.endsWith('"')) {
        s = s.mid(1, s.size() - 2);
        s.replace("\\\"", "\"").replace("\\\\", "\\");
    }
    return s;
}

QList<QByteArray> splitArgs(const QByteArray &line)
{
    QList<QByteArray> out;
    QByteArray cur;
    bool q = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\\' && q && i + 1 < line.size()) {
            cur += c;
            cur += line[++i];
            continue;
        }
        if (c == '"')
            q = !q;
        if (c == ' ' && !q) {
            out.append(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.append(cur);
    return out;
}

template<typename Conn, typename Handler>
void serve(QTcpServer &server, QObject *owner, Handler handler, const QByteArray &greeting)
{
    QObject::connect(&server, &QTcpServer::newConnection, owner, [&server, handler, greeting] {
        while (QTcpSocket *s = server.nextPendingConnection()) {
            auto conn = std::make_shared<Conn>();
            auto buffer = std::make_shared<QByteArray>();
            QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            QObject::connect(s, &QTcpSocket::readyRead, s, [s, conn, buffer, handler] {
                *buffer += s->readAll();
                qsizetype nl;
                while ((nl = buffer->indexOf('\n')) >= 0) {
                    QByteArray line = buffer->left(nl);
                    buffer->remove(0, nl + 1);
                    if (line.endsWith('\r'))
                        line.chop(1);
                    handler(s, *conn, line);
                    if (s->state() != QAbstractSocket::ConnectedState)
                        return;
                }
            });
            s->write(greeting);
        }
    });
}

} // namespace

FakeMailServer::FakeMailServer()
{
    m_smtp.listen(QHostAddress::LocalHost);
    m_imap.listen(QHostAddress::LocalHost);
    m_pop3.listen(QHostAddress::LocalHost);
    serve<SmtpConn>(m_smtp, this,
                    [this](QTcpSocket *s, SmtpConn &c, const QByteArray &l) { smtpLine(s, c, l); },
                    "220 fake ESMTP\r\n");
    serve<Pop3Conn>(m_pop3, this,
                    [this](QTcpSocket *s, Pop3Conn &c, const QByteArray &l) { pop3Line(s, c, l); },
                    "+OK fake POP3\r\n");
    // The IMAP greeting depends on switches set after construction, so it
    // carries no capabilities and the client must ask.
    serve<ImapConn>(m_imap, this,
                    [this](QTcpSocket *s, ImapConn &c, const QByteArray &l) { imapLine(s, c, l); },
                    "* OK fake IMAP ready\r\n");
}

FakeMailServer::~FakeMailServer() = default;

void FakeMailServer::addUser(const QString &addr, const QString &password)
{
    m_passwords.insert(addr.toLower(), password);
    m_boxes[addr.toLower()];
    m_nextUid.insert(addr.toLower(), 1);
}

void FakeMailServer::deliver(const QString &addr, const QByteArray &raw)
{
    const QString key = addr.toLower();
    if (!m_boxes.contains(key))
        return;
    m_boxes[key].append({m_nextUid[key]++, raw, false});
    for (const auto &[sock, user] : std::as_const(m_idlers)) {
        if (user == key)
            sock->write("* " + QByteArray::number(m_boxes[key].size()) + " EXISTS\r\n");
    }
}

QList<QByteArray> FakeMailServer::mailbox(const QString &addr) const
{
    QList<QByteArray> out;
    for (const Stored &m : m_boxes.value(addr.toLower())) {
        if (!m.deleted)
            out.append(m.data);
    }
    return out;
}

bool FakeMailServer::checkPlain(const QByteArray &b64, QString *user) const
{
    const QList<QByteArray> parts = QByteArray::fromBase64(b64).split('\0');
    if (parts.size() != 3)
        return false;
    const QString u = QString::fromUtf8(parts[1]).toLower();
    if (!m_passwords.contains(u) || m_passwords.value(u) != QString::fromUtf8(parts[2]))
        return false;
    *user = u;
    return true;
}

void FakeMailServer::imapLine(QTcpSocket *s, ImapConn &c, const QByteArray &line)
{
    if (!c.idleTag.isEmpty()) {
        if (line.trimmed().toUpper() == "DONE") {
            m_idlers.removeIf([s](const auto &p) { return p.first == s; });
            s->write(c.idleTag + " OK IDLE done\r\n");
            c.idleTag.clear();
        }
        return;
    }
    if (!c.pendingAuthTag.isEmpty()) {
        const QByteArray tag = c.pendingAuthTag;
        c.pendingAuthTag.clear();
        s->write(checkPlain(line.trimmed(), &c.user) ? tag + " OK logged in\r\n" : tag + " NO bad credentials\r\n");
        return;
    }
    const QList<QByteArray> a = splitArgs(line);
    if (a.size() < 2)
        return;
    const QByteArray tag = a[0];
    QByteArray cmd = a[1].toUpper();
    QList<QByteArray> args = a.mid(2);
    if (cmd == "UID" && !args.isEmpty()) {
        cmd = "UID " + args.takeFirst().toUpper();
    }
    QList<Stored> &box = m_boxes[c.user];

    if (cmd == "CAPABILITY") {
        QByteArray caps = "* CAPABILITY IMAP4rev1";
        if (imapAuthPlain)
            caps += " AUTH=PLAIN SASL-IR";
        if (imapIdle)
            caps += " IDLE";
        if (imapUidPlus)
            caps += " UIDPLUS";
        s->write(caps + "\r\n" + tag + " OK done\r\n");
    } else if (cmd == "LOGIN") {
        const QString u = QString::fromUtf8(unquote(args.value(0))).toLower();
        const QString p = QString::fromUtf8(unquote(args.value(1)));
        if (m_passwords.contains(u) && m_passwords.value(u) == p) {
            c.user = u;
            s->write(tag + " OK logged in\r\n");
        } else {
            s->write(tag + " NO [AUTHENTICATIONFAILED] bad credentials\r\n");
        }
    } else if (cmd == "AUTHENTICATE") {
        if (args.size() >= 2) {
            s->write(checkPlain(args[1], &c.user) ? tag + " OK logged in\r\n" : tag + " NO bad credentials\r\n");
        } else {
            c.pendingAuthTag = tag;
            s->write("+ \r\n");
        }
    } else if (c.user.isEmpty()) {
        s->write(tag + " BAD not logged in\r\n");
    } else if (cmd == "SELECT") {
        quint32 exists = 0;
        for (const Stored &m : box)
            exists += m.deleted ? 0 : 1;
        s->write("* " + QByteArray::number(exists) + " EXISTS\r\n* OK [UIDVALIDITY "
                 + QByteArray::number(uidValidity) + "] ok\r\n* OK [UIDNEXT "
                 + QByteArray::number(m_nextUid.value(c.user)) + "] ok\r\n" + tag + " OK [READ-WRITE] selected\r\n");
    } else if (cmd == "UID SEARCH") {
        // ALL, or UID a:b where b may be "*". As RFC 3501 says, "a:*" also
        // matches the last message when a is above every UID.
        quint32 lo = 1, hi = UINT32_MAX, last = 0;
        for (const Stored &m : box)
            last = m.deleted ? last : qMax(last, m.uid);
        if (args.value(0).toUpper() == "UID") {
            const QList<QByteArray> r = args.value(1).split(':');
            lo = r.value(0).toUInt();
            hi = r.value(1) == "*" ? UINT32_MAX : r.value(1).toUInt();
            if (r.value(1) == "*" && lo > last)
                lo = last;
        }
        QByteArray out = "* SEARCH";
        for (const Stored &m : box) {
            if (!m.deleted && m.uid >= lo && m.uid <= hi)
                out += ' ' + QByteArray::number(m.uid);
        }
        ++searchCount;
        s->write(out + "\r\n" + tag + " OK done\r\n");
    } else if (cmd == "UID FETCH") {
        const quint32 uid = args.value(0).toUInt();
        ++fetchCount;
        int seq = 0;
        for (const Stored &m : box) {
            if (m.deleted)
                continue;
            ++seq;
            if (m.uid == uid) {
                s->write("* " + QByteArray::number(seq) + " FETCH (UID " + QByteArray::number(uid) + " BODY[] {"
                         + QByteArray::number(m.data.size()) + "}\r\n" + m.data + ")\r\n");
            }
        }
        s->write(tag + " OK done\r\n");
    } else if (cmd == "UID STORE") {
        for (const QByteArray &u : args.value(0).split(',')) {
            for (Stored &m : box) {
                if (m.uid == u.toUInt())
                    m.deleted = true;
            }
        }
        s->write(tag + " OK done\r\n");
    } else if (cmd == "UID EXPUNGE" || cmd == "EXPUNGE") {
        ++expungeCount;
        s->write(tag + " OK done\r\n");
    } else if (cmd == "IDLE") {
        c.idleTag = tag;
        m_idlers.append({s, c.user});
        s->write("+ idling\r\n");
    } else if (cmd == "NOOP") {
        s->write(tag + " OK done\r\n");
    } else if (cmd == "LOGOUT") {
        s->write("* BYE\r\n" + tag + " OK bye\r\n");
        s->disconnectFromHost();
    } else {
        s->write(tag + " BAD unknown\r\n");
    }
}

void FakeMailServer::pop3Line(QTcpSocket *s, Pop3Conn &c, const QByteArray &line)
{
    const QList<QByteArray> a = line.split(' ');
    const QByteArray cmd = a.value(0).toUpper();
    QList<Stored> &box = m_boxes[c.user];
    // POP3 numbers the messages present at login; deleted ones keep a slot.
    auto present = [&]() {
        QList<Stored *> out;
        for (Stored &m : box) {
            if (!m.deleted)
                out.append(&m);
        }
        return out;
    };
    if (cmd == "CAPA") {
        s->write(QByteArray("+OK\r\nUIDL\r\nUSER\r\n") + (pop3SaslPlain ? "SASL PLAIN\r\n" : "") + ".\r\n");
    } else if (cmd == "USER") {
        c.pendingUser = QString::fromUtf8(a.value(1)).toLower();
        s->write("+OK\r\n");
    } else if (cmd == "PASS") {
        if (m_passwords.value(c.pendingUser) == QString::fromUtf8(line.mid(5)) && m_passwords.contains(c.pendingUser)) {
            c.user = c.pendingUser;
            s->write("+OK logged in\r\n");
        } else {
            s->write("-ERR bad credentials\r\n");
        }
    } else if (cmd == "AUTH") {
        s->write(checkPlain(a.value(2), &c.user) ? "+OK logged in\r\n" : "-ERR bad credentials\r\n");
    } else if (c.user.isEmpty()) {
        s->write("-ERR not logged in\r\n");
    } else if (cmd == "UIDL") {
        QByteArray out = "+OK\r\n";
        const auto msgs = present();
        for (qsizetype i = 0; i < msgs.size(); ++i)
            out += QByteArray::number(i + 1) + " uid-" + QByteArray::number(msgs[i]->uid) + "\r\n";
        s->write(out + ".\r\n");
    } else if (cmd == "RETR") {
        const auto msgs = present();
        const int n = a.value(1).toInt();
        if (n < 1 || n > msgs.size()) {
            s->write("-ERR no such message\r\n");
            return;
        }
        QByteArray out = "+OK\r\n";
        QByteArray data = msgs[n - 1]->data;
        if (data.endsWith('\n'))
            data.chop(1);
        for (QByteArray l : data.split('\n')) {
            if (l.endsWith('\r'))
                l.chop(1);
            if (l.startsWith('.'))
                l.prepend('.');
            out += l + "\r\n";
        }
        s->write(out + ".\r\n");
    } else if (cmd == "DELE") {
        c.dele.append(a.value(1).toInt());
        s->write("+OK\r\n");
    } else if (cmd == "QUIT") {
        const auto msgs = present();
        for (int n : std::as_const(c.dele)) {
            if (n >= 1 && n <= msgs.size())
                msgs[n - 1]->deleted = true;
        }
        s->write("+OK bye\r\n");
        s->disconnectFromHost();
    } else {
        s->write("-ERR unknown\r\n");
    }
}

void FakeMailServer::smtpLine(QTcpSocket *s, SmtpConn &c, const QByteArray &line)
{
    if (c.inData) {
        if (line == ".") {
            c.inData = false;
            m_envelopes.append({c.from, c.rcpts, c.data});
            for (const QString &r : std::as_const(c.rcpts))
                deliver(r, c.data);
            c.data.clear();
            c.rcpts.clear();
            s->write("250 queued\r\n");
            return;
        }
        c.data += (line.startsWith('.') ? line.mid(1) : line) + "\r\n";
        return;
    }
    const QByteArray upper = line.toUpper();
    if (upper.startsWith("EHLO") || upper.startsWith("HELO")) {
        s->write("250-fake\r\n250-AUTH PLAIN LOGIN\r\n250 8BITMIME\r\n");
    } else if (upper.startsWith("AUTH PLAIN ")) {
        QString user;
        c.authed = checkPlain(line.mid(11), &user);
        s->write(c.authed ? "235 ok\r\n" : "535 bad credentials\r\n");
    } else if (upper.startsWith("MAIL FROM:")) {
        if (!c.authed) {
            s->write("530 authenticate first\r\n");
            return;
        }
        c.from = QString::fromUtf8(line.mid(10).split(' ').first()).remove(u'<').remove(u'>');
        s->write("250 ok\r\n");
    } else if (upper.startsWith("RCPT TO:")) {
        const QString r = QString::fromUtf8(line.mid(8)).remove(u'<').remove(u'>').trimmed();
        if (r.startsWith(QLatin1String("reject"))) {
            s->write("550 no such user\r\n");
            return;
        }
        c.rcpts.append(r);
        s->write("250 ok\r\n");
    } else if (upper == "DATA") {
        c.inData = true;
        s->write("354 go ahead\r\n");
    } else if (upper == "RSET") {
        c.rcpts.clear();
        s->write("250 ok\r\n");
    } else if (upper == "QUIT") {
        s->write("221 bye\r\n");
        s->disconnectFromHost();
    } else {
        s->write("502 unknown\r\n");
    }
}
