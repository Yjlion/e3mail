// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "SyncSocket.h"

#include <QList>
#include <QSet>

namespace e3::net {

// IMAP4rev1 as a transport: one folder, read by UID, deleted when retention
// says so. Nothing is ever written back to the server.
class ImapClient
{
public:
    struct Mailbox
    {
        quint32 uidValidity = 0;
        quint32 uidNext = 0;
        quint32 exists = 0;
    };

    void connect(const ServerSettings &server);
    Mailbox select(const QString &mailbox = QStringLiteral("INBOX"));
    QList<quint32> uids();                       // UID SEARCH ALL, ascending
    QByteArray fetch(quint32 uid);               // BODY.PEEK[], never sets \Seen
    void remove(const QList<quint32> &uids);     // \Deleted, then (UID) EXPUNGE
    // IDLE until the server reports a change, `stop()` is true or maxMs
    // passes. Returns true if the mailbox changed. Polls with NOOP when the
    // server has no IDLE.
    bool idle(int maxMs, const std::function<bool()> &stop);
    void logout();

    bool hasCapability(const QByteArray &cap) const { return m_caps.contains(cap.toUpper()); }

    // Quotes a string for an IMAP command, or returns nullopt-equivalent empty
    // when it needs a literal (CR, LF, NUL or 8-bit).
    static QByteArray quote(const QByteArray &s);

private:
    struct Response
    {
        QList<QByteArray> untagged;              // "* ..." lines, literals inlined
        QList<QList<QByteArray>> literals;       // per untagged line
        QByteArray status;                       // OK / NO / BAD
        QByteArray text;
    };
    QByteArray nextTag();
    Response command(const QByteArray &cmd);
    Response readUntilTagged(const QByteArray &tag);
    void readUntagged(Response &r, QByteArray line);
    void parseCapabilities(const QByteArray &line);
    static void throwIfFailed(const Response &r, const char *what);

    SyncSocket m_socket{120000};
    QSet<QByteArray> m_caps;
    int m_tag = 0;
};

} // namespace e3::net
