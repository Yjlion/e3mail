// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "DeviceTransport.h"

#include <QHash>
#include <QSet>

#include <atomic>

struct e3p2p_node;

namespace e3 {
class Database;
}

namespace e3::sync {

// DeviceTransport over Iroh (src/p2p, ADR 0010). Each device is an Iroh
// endpoint named by its public key; only the endpoints of paired devices
// (devices.node_id) may connect. On a new connection both sides say which
// device they are; a device id that does not match the endpoint it came
// from is refused. Lives on the thread that created it.
class IrohTransport : public DeviceTransport
{
    Q_OBJECT
public:
    enum Frame : quint8 {
        Hello = 1,       // {"device": id}
        Sync = 2,        // a SyncSession message
        BlobRequest = 3, // the blob's name
        Blob = 4,        // 64 bytes of name, then the data
        BlobMissing = 5, // the blob's name
        // 16 and up: pairing (Pairing), on connections from no paired device
    };
    enum class Relay { Default, Custom, Off };

    struct Options
    {
        QByteArray secret; // 32 bytes
        Relay relay = Relay::Default;
        QString relayUrl;
    };

    // `db` is read for the paired devices; `self` is this device's id.
    IrohTransport(Database &db, const QString &self, const Options &options, QObject *parent = nullptr);
    ~IrohTransport() override;

    bool isRunning() const { return m_node != nullptr; }
    QString nodeId() const;
    // Where this endpoint can be reached now, for a peer to dial.
    QString addr() const;
    bool waitOnline(int ms);
    static QByteArray generateSecret();

    // Lets in exactly the paired devices' endpoints; call after pairing.
    void refreshPeers();
    // Dials every paired device not connected now.
    void dialPaired();
    // Dials an endpoint by the address it gave; for pairing.
    quint64 dial(const QString &addr);
    // While on, connections from any endpoint are let in (pairing).
    void acceptAny(bool on);
    bool isConnected(const QString &device) const { return m_byDevice.contains(device); }
    QStringList connectedDevices() const { return m_byDevice.keys(); }

    void send(const QString &device, const QByteArray &message) override;
    void requestBlob(const QString &device, const QString &name) override;
    void provideBlob(const QString &device, const QString &name, const QByteArray &data) override;

    // Pairing speaks on connections that are no paired device's.
    void sendFrame(quint64 conn, quint8 kind, const QByteArray &data);
    void close(quint64 conn);

Q_SIGNALS:
    // A connection from or to an endpoint that is not a paired device, or
    // that has not said which device it is: frames 16 and up.
    void strangerFrame(quint64 conn, const QString &peer, quint8 kind, const QByteArray &data);
    void strangerConnected(quint64 conn, const QString &peer, bool incoming);
    void strangerClosed(quint64 conn, const QString &reason);

private:
    struct Conn
    {
        QString peer;
        bool incoming = false;
        QString device; // once its Hello matched a paired device
    };
    static void wake(void *self);
    void poll();
    void onFrame(quint64 conn, quint8 kind, const QByteArray &data);
    void onHello(quint64 conn, const QByteArray &data);
    void onClosed(quint64 conn, const QString &why);
    bool sendTo(const QString &device, quint8 kind, const QByteArray &data);

    Database &m_db;
    QString m_self;
    e3p2p_node *m_node = nullptr;
    std::atomic<bool> m_wakePending{false};
    QHash<quint64, Conn> m_conns;
    QHash<QString, quint64> m_byDevice;
    QSet<quint64> m_dialing; // our own dials, still connecting
};

} // namespace e3::sync
