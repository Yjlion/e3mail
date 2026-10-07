// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "SyncSocket.h"

#include <QList>
#include <QSet>

#include <optional>

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
    // UID SEARCH UID <range>, e.g. "5:*" or "1:99", ascending. Only UIDs in
    // the range are returned: "n:*" always matches the last message.
    QList<quint32> uids(quint32 first, std::optional<quint32> last = std::nullopt);
    // BODY.PEEK[], never sets \Seen. Nothing when the message is gone, which
    // another client may have deleted since it was listed.
    std::optional<QByteArray> fetch(quint32 uid);
    void remove(const QList<quint32> &uids);     // \Deleted, then (UID) EXPUNGE
    // IDLE until the server reports a change, `stop()` is true or maxMs
    // passes. Returns true if the mailbox changed. Polls with NOOP when the
    // server has no IDLE. Stopped mid-IDLE, it closes the connection.
    bool idle(int maxMs, const std::function<bool()> &stop);
    void logout();
    void close(); // without LOGOUT: nothing waits on the server


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
