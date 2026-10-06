// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "SyncSocket.h"

#include <QSet>
#include <QStringList>

namespace e3::net {

class SmtpClient
{
public:
    SmtpClient() = default;

    // Greeting, EHLO, STARTTLS when asked for, AUTH when credentials are set.
    void connect(const ServerSettings &server);

    // Sends one message. Returns the recipients the server refused; throws if
    // it refused all of them or the message itself.
    QStringList send(const QString &from, const QStringList &recipients, const QByteArray &message);

    void quit();

    // Dot-stuffing and CRLF normalisation for DATA.
    static QByteArray prepareData(const QByteArray &message);

private:
    struct Reply
    {
        int code = 0;
        QList<QByteArray> lines;
        QString text() const;
    };
    Reply readReply();
    Reply command(const QByteArray &line);
    void expect(const Reply &r, int lo, int hi, const char *what);
    void ehlo();

    SyncSocket m_socket;
    QSet<QByteArray> m_caps; // upper case EHLO keywords
    QList<QByteArray> m_authMechs;
};

} // namespace e3::net
