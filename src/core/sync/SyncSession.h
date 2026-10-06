// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "OpApply.h"

#include <QObject>
#include <QSet>

namespace e3::sync {

class DeviceTransport;

// Keeps this mailbox's op-log in step with the paired devices over a
// transport. On connect each side says which ops it has; each sends the
// other what it lacks, and passes on ops from devices the other cannot reach.
// Messages a peer stored and this device lacks are fetched as raw blobs and
// stored through the ordinary receive path.
//
// Only paired devices are listened to. Wire messages are versioned JSON.
class SyncSession : public QObject
{
    Q_OBJECT
public:
    SyncSession(MailContext &ctx, DeviceTransport &transport, QObject *parent = nullptr);

    // Records another device of this account. Pairing proper (QR, snapshot)
    // is a later step; this is what it ends with.
    static void pair(MailContext &ctx, const QString &device, const QString &name);
    static void unpair(MailContext &ctx, const QString &device);
    static bool isPaired(MailContext &ctx, const QString &device);

    // Sends connected devices what they lack. Call after local changes.
    void push();

    static constexpr int kBatch = 500;

Q_SIGNALS:
    // The mailbox changed because of another device.
    void changed();

private:
    void onConnected(const QString &device);
    void onDisconnected(const QString &device);
    void onReceived(const QString &device, const QByteArray &message);
    void onBlobRequested(const QString &device, const QString &name);
    void onBlobReceived(const QString &device, const QString &name, const QByteArray &data);

    void sendHello(const QString &device);
    void sendMissing(const QString &device);
    void handleOps(const QString &device, const QList<Op> &ops);

    MailContext &m_ctx;
    DeviceTransport &m_transport;
    QSet<QString> m_peers;
    // What each connected peer is known to hold, so push() sends only news.
    QHash<QString, OpApply::Versions> m_peerHave;
};

} // namespace e3::sync
