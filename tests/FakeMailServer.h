// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QTcpServer>

class QTcpSocket;

// An in-process mail system for tests: SMTP delivers into per-user mailboxes,
// which IMAP and POP3 then serve. Plain TCP on localhost; runs on the thread
// that created it, and is serviced by any event loop on that thread, including
// the local loops the clients spin while they wait.
class FakeMailServer : public QObject
{
    Q_OBJECT
public:
    struct Stored
    {
        quint32 uid;
        QByteArray data;
        bool deleted = false;
    };
    struct Envelope
    {
        QString from;
        QStringList rcpts;
        QByteArray data;
    };

    FakeMailServer();
    ~FakeMailServer() override;

    quint16 smtpPort() const { return m_smtp.serverPort(); }
    quint16 imapPort() const { return m_imap.serverPort(); }
    quint16 pop3Port() const { return m_pop3.serverPort(); }

    void addUser(const QString &addr, const QString &password);
    void deliver(const QString &addr, const QByteArray &raw);
    QList<QByteArray> mailbox(const QString &addr) const;
    const QList<Envelope> &envelopes() const { return m_envelopes; }

    // Behaviour switches, so client fallbacks get exercised.
    bool imapIdle = true;
    bool imapUidPlus = true;
    bool imapAuthPlain = true;
    bool pop3SaslPlain = false;
    quint32 uidValidity = 777;
    int expungeCount = 0;

private:
    struct ImapConn;
    struct Pop3Conn;
    struct SmtpConn;
    void onImap();
    void onPop3();
    void onSmtp();
    void imapLine(QTcpSocket *s, ImapConn &c, const QByteArray &line);
    void pop3Line(QTcpSocket *s, Pop3Conn &c, const QByteArray &line);
    void smtpLine(QTcpSocket *s, SmtpConn &c, const QByteArray &line);
    bool checkPlain(const QByteArray &b64, QString *user) const;

    QTcpServer m_smtp, m_imap, m_pop3;
    QHash<QString, QString> m_passwords;
    QHash<QString, QList<Stored>> m_boxes;
    QHash<QString, quint32> m_nextUid;
    QList<Envelope> m_envelopes;
    QList<QPair<QTcpSocket *, QString>> m_idlers;
};
