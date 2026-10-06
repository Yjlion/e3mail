// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "net/SyncSocket.h"

#include <QList>
#include <QObject>
#include <QString>

class QNetworkAccessManager;

namespace e3 {

// Finds server settings for an address: Thunderbird's ISPDB first, then the
// conventional host names. The result is a suggestion the setup form shows and
// the user can change; nothing here connects to a mail server.
class Autoconfig : public QObject
{
    Q_OBJECT
public:
    struct Server
    {
        QString protocol; // imap | pop3 | smtp
        QString host;
        quint16 port = 0;
        net::Security security = net::Security::Ssl;
        QString username; // with %EMAILADDRESS% and %EMAILLOCALPART% expanded
    };
    struct Result
    {
        QList<Server> incoming; // IMAP before POP3
        Server smtp;
        QString source;         // "ispdb" | "guess"
    };

    explicit Autoconfig(QObject *parent = nullptr);
    void lookup(const QString &addr);

    // Exposed for tests.
    static Result parseIspdb(const QByteArray &xml, const QString &addr);
    static Result guess(const QString &addr);

Q_SIGNALS:
    void finished(const e3::Autoconfig::Result &result);

private:
    QNetworkAccessManager *m_nam;
};

} // namespace e3
