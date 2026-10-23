// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTemporaryDir>

#include <QJsonObject>

#include <memory>
#include <optional>

namespace e3 {

class Account;
class AccountManager;
class Database;

namespace sync {
class IrohTransport;
}

// Pairing a new device with an account that already exists on another
// (ADR 0010, amended 2026-10-11). The existing device makes a one-time code;
// the new device dials it, says who it is, and waits; a person on the
// existing device allows it. The new device then pulls a snapshot (the
// database and every blob, checked by SHA-256, and the passwords) over the
// authenticated Iroh connection, becomes a device of the account with an id
// and key of its own, and both record each other. A `device.pair` op tells
// every other device.
namespace pairing {

enum Frame : quint8 {
    Request = 16,  // joiner: {token, name, device}
    Denied = 17,   // either: {reason}
    Manifest = 18, // offerer: what the snapshot holds
    Get = 19,      // joiner: {what: "db"|"blob", name, offset}
    Chunk = 20,    // offerer: [what: u8][name: 64][offset: u64 BE][bytes]
    Done = 21,     // joiner: everything arrived and checked
};

struct Code
{
    QString addr;    // the offering endpoint, as IrohTransport::addr gives it
    QByteArray token;
    QString account; // the address, to show before joining
    QString relay;   // the offering device's relay setting
};
QString formatCode(const Code &c);
std::optional<Code> parseCode(const QString &text);

} // namespace pairing

// The existing device's side. Lives as long as the dialog showing the code.
class PairingOffer : public QObject
{
    Q_OBJECT
public:
    static constexpr int kValidMinutes = 10;

    explicit PairingOffer(Account &account, QObject *parent = nullptr);
    ~PairingOffer() override;

    bool isReady() const { return !m_code.isEmpty(); }
    QString code() const { return m_code; }
    // The name the new device gave, while it waits for allow() or deny().
    QString requester() const { return m_requester; }
    void allow();
    void deny();

Q_SIGNALS:
    void requested(const QString &deviceName);
    void progress(int percent);
    // ok: the new device has everything and is paired.
    void finished(bool ok, const QString &message);

private:
    void onFrame(quint64 conn, const QString &peer, quint8 kind, const QByteArray &data);
    void onClosed(quint64 conn, const QString &reason);
    void serve(const QJsonObject &get);
    void fail(const QString &message);

    QPointer<Account> m_account;
    sync::IrohTransport *m_transport = nullptr;
    QString m_code;
    QByteArray m_token;
    QDateTime m_expires;
    bool m_used = false;
    quint64 m_conn = 0;
    QString m_peer, m_requester, m_device;
    std::unique_ptr<QTemporaryDir> m_tmp;
    QString m_dbPath;
    QByteArray m_snapshotKey;
    qint64 m_total = 0, m_served = 0;
    QString m_cachedBlob;
    QByteArray m_cachedData;
};

// The new device's side: from a code to a configured account.
class PairingJoin : public QObject
{
    Q_OBJECT
public:
    PairingJoin(AccountManager &manager, const QString &code, const QString &deviceName, QObject *parent = nullptr);
    ~PairingJoin() override;

    // The account the code is for, to show while joining.
    QString account() const { return m_code.account; }
    bool isValid() const { return m_valid; }

Q_SIGNALS:
    void waiting();          // the other device has been asked
    void progress(int percent);
    void finished(bool ok, const QString &message, int accountId);

private:
    void onConnected(quint64 conn, const QString &peer, bool incoming);
    void onFrame(quint64 conn, const QString &peer, quint8 kind, const QByteArray &data);
    void onClosed(quint64 conn, const QString &reason);
    void onManifest(const QByteArray &data);
    void onChunk(const QByteArray &data);
    void requestMore();
    void complete();
    void fail(const QString &message);

    AccountManager &m_manager;
    pairing::Code m_code;
    QString m_name;
    bool m_valid = false;
    bool m_finished = false;
    QByteArray m_secret;
    QString m_device;
    std::unique_ptr<QTemporaryDir> m_work;
    std::unique_ptr<Database> m_scratchDb; // what the joining endpoint consults
    std::unique_ptr<sync::IrohTransport> m_transport;
    quint64 m_conn = 0;
    QJsonObject m_manifest;
    QString m_stage;
    QByteArray m_stageKey;
    QFile m_db;
    // What is still to ask for, in order: the database in pieces, then each
    // blob in pieces. Answers come back in the same order.
    struct Piece
    {
        bool blob = false;
        QString name;
        qint64 offset = 0;
        qint64 size = 0;      // of the whole blob, for blobs
    };
    QList<Piece> m_toAsk;
    QList<Piece> m_asked;
    QByteArray m_blobData;
    QString m_dbSha;
    qint64 m_total = 0, m_received = 0;
};

} // namespace e3
