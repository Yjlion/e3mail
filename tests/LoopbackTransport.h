// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "sync/DeviceTransport.h"

#include <QHash>
#include <QSet>

class LoopbackNetwork;

// One device's end of the in-process network.
class LoopbackTransport : public e3::sync::DeviceTransport
{
    Q_OBJECT
public:
    LoopbackTransport(LoopbackNetwork &net, const QString &self);

    void send(const QString &device, const QByteArray &message) override;
    void requestBlob(const QString &device, const QString &name) override;
    void provideBlob(const QString &device, const QString &name, const QByteArray &data) override;

    QString self() const { return m_self; }

private:
    LoopbackNetwork &m_net;
    QString m_self;
};

// An in-process stand-in for Iroh, for tests. Delivery is queued, as over a
// real network, and settle() runs the event loop until nothing is in flight.
// It can drop messages and withhold blobs, to exercise recovery.
class LoopbackNetwork
{
public:
    ~LoopbackNetwork();

    LoopbackTransport *endpoint(const QString &device);
    void link(const QString &a, const QString &b);
    void unlink(const QString &a, const QString &b);
    bool linked(const QString &a, const QString &b) const;
    // Runs the event loop until no delivery is pending.
    void settle();

    // The next `n` messages from a to b are lost.
    void drop(const QString &from, const QString &to, int n) { m_drop[from + u'>' + to] += n; }
    // When set, every blob request is answered "not here".
    bool withholdBlobs = false;
    int delivered = 0;

private:
    friend class LoopbackTransport;
    void post(std::function<void()> fn);

    QHash<QString, LoopbackTransport *> m_endpoints;
    QSet<QString> m_links;
    QHash<QString, int> m_drop;
    int m_pending = 0;
};
