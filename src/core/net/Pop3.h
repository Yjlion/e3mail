// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "SyncSocket.h"

#include <QList>

namespace e3::net {

// POP3 (RFC 1939, STLS from RFC 2595, AUTH PLAIN from RFC 5034). POP3 is a
// natural fit for transport-only mail: download, then delete. Messages are
// identified by UIDL, because message numbers change between sessions.
class Pop3Client
{
public:
    struct Entry
    {
        int number;
        QString uidl;
    };

    void connect(const ServerSettings &server);
    QList<Entry> list();            // UIDL
    QByteArray retrieve(int number);
    void remove(int number);        // DELE; takes effect at quit()
    void quit();                    // commits deletions

private:
    QByteArray command(const QByteArray &line, const char *what);
    QByteArray readMultiline();

    SyncSocket m_socket{120000};
    QList<QByteArray> m_caps;
};

} // namespace e3::net
