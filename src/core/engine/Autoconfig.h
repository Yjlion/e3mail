// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "net/SyncSocket.h"

#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

class QNetworkAccessManager;
class QTimer;
class QUrl;

namespace e3 {

// Finds server settings for an address (ADR 0015). In order of trust: the
// provider's own autoconfig file, Thunderbird's ISPDB, Microsoft Autodiscover,
// DNS SRV records (RFC 6186, RFC 8314), ISPDB for the domain's MX host, and
// last the conventional host names. All are asked at once; the first in that
// order with both an incoming and an outgoing server wins.
//
// The result is a suggestion the setup form shows and the user can change.
// Nothing here connects to a mail server, sends a password, or suggests a
// cleartext connection.
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
        QString source;         // provider | ispdb | autodiscover | srv | mx | guess
        // From unauthenticated DNS, naming hosts outside the address's
        // domain: the form shows them before connecting (RFC 6186 §6).
        bool needsReview = false;

        bool complete() const { return !incoming.isEmpty() && !smtp.host.isEmpty(); }
    };
    // A DNS answer: SRV records, or MX records with the preference in
    // `priority` and no port.
    struct DnsRecord
    {
        QString target;
        quint16 port = 0;
        quint16 priority = 0;
        quint16 weight = 0;
    };
    enum class DnsType { Srv, Mx };

    static constexpr int kDeadlineMs = 10000;

    explicit Autoconfig(QObject *parent = nullptr);
    ~Autoconfig() override;
    // A new lookup abandons the one before it.
    void lookup(const QString &addr);

    // Exposed for tests.
    static Result parseAutoconfig(const QByteArray &xml, const QString &addr);
    // `redirectAddr`, if given, receives the address an Autodiscover
    // response redirects to.
    static Result parseAutodiscover(const QByteArray &xml, const QString &addr, QString *redirectAddr = nullptr);
    // Keyed by service, e.g. "_imaps._tcp".
    static Result fromSrv(const QHash<QString, QList<DnsRecord>> &records, const QString &addr);
    static Result guess(const QString &addr);
    // Where to ask ISPDB about a mail exchanger: aspmx.l.google.com gives
    // l.google.com, then google.com.
    static QStringList mxDomains(const QString &mxHost);
    static QByteArray autodiscoverRequest(const QString &addr);

Q_SIGNALS:
    void finished(const e3::Autoconfig::Result &result);

protected:
    // The network, overridden by tests. `done` gets the body of a successful
    // (200) response, or nothing. A non-empty `postBody` makes it a POST.
    virtual void fetch(const QUrl &url, const QByteArray &postBody,
                       std::function<void(std::optional<QByteArray>)> done);
    // `done` gets the records, empty when there are none or on any error.
    virtual void resolve(DnsType type, const QString &name, std::function<void(QList<DnsRecord>)> done);
    void setDeadline(int ms);

private:
    enum Source { Provider, Ispdb, Autodiscover, Srv, Mx, SourceCount };

    bool current(quint64 gen) const;
    void settle(bool deadline);
    void finish(Source s, Result r);
    // Calls `done` with the first complete result among `urls`, in order.
    void fetchFirst(quint64 gen, const QList<QUrl> &urls, const QByteArray &postBody,
                    std::function<Result(const QByteArray &)> parse, std::function<void(Result)> done);
    void startProvider(quint64 gen);
    void startIspdb(quint64 gen);
    void startAutodiscover(quint64 gen);
    void startSrv(quint64 gen);
    void startMx(quint64 gen);

    QNetworkAccessManager *m_nam;
    QTimer *m_deadline;
    int m_deadlineMs = kDeadlineMs;
    quint64 m_gen = 0; // which lookup a callback belongs to
    QString m_addr, m_domain;
    std::optional<Result> m_results[SourceCount];
    bool m_done = true;
};

} // namespace e3
