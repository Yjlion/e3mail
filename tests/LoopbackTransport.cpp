// SPDX-License-Identifier: MPL-2.0
#include "LoopbackTransport.h"

#include <QCoreApplication>
#include <QTimer>

namespace {
QString linkKey(const QString &a, const QString &b)
{
    return a < b ? a + u'|' + b : b + u'|' + a;
}
} // namespace

LoopbackTransport::LoopbackTransport(LoopbackNetwork &net, const QString &self) : m_net(net), m_self(self) {}

void LoopbackTransport::send(const QString &device, const QByteArray &message)
{
    if (!m_net.linked(m_self, device))
        return;
    int &drop = m_net.m_drop[m_self + u'>' + device];
    if (drop > 0) {
        --drop;
        return;
    }
    const QString from = m_self;
    m_net.post([this, device, from, message] {
        if (m_net.linked(from, device)) {
            ++m_net.delivered;
            Q_EMIT m_net.endpoint(device)->received(from, message);
        }
    });
}

void LoopbackTransport::requestBlob(const QString &device, const QString &name)
{
    if (!m_net.linked(m_self, device))
        return;
    const QString from = m_self;
    m_net.post([this, device, from, name] {
        if (m_net.withholdBlobs)
            Q_EMIT blobMissing(device, name);
        else
            Q_EMIT m_net.endpoint(device)->blobRequested(from, name);
    });
}

void LoopbackTransport::provideBlob(const QString &device, const QString &name, const QByteArray &data)
{
    const QString from = m_self;
    m_net.post([this, device, from, name, data] {
        LoopbackTransport *peer = m_net.endpoint(device);
        if (data.isEmpty())
            Q_EMIT peer->blobMissing(from, name);
        else
            Q_EMIT peer->blobReceived(from, name, data);
    });
}

LoopbackNetwork::~LoopbackNetwork()
{
    qDeleteAll(m_endpoints);
}

LoopbackTransport *LoopbackNetwork::endpoint(const QString &device)
{
    LoopbackTransport *&t = m_endpoints[device];
    if (!t)
        t = new LoopbackTransport(*this, device);
    return t;
}

void LoopbackNetwork::link(const QString &a, const QString &b)
{
    m_links.insert(linkKey(a, b));
    LoopbackTransport *ta = endpoint(a);
    LoopbackTransport *tb = endpoint(b);
    post([ta, tb, a, b] {
        Q_EMIT ta->connected(b);
        Q_EMIT tb->connected(a);
    });
}

void LoopbackNetwork::unlink(const QString &a, const QString &b)
{
    m_links.remove(linkKey(a, b));
    Q_EMIT endpoint(a)->disconnected(b);
    Q_EMIT endpoint(b)->disconnected(a);
}

bool LoopbackNetwork::linked(const QString &a, const QString &b) const
{
    return m_links.contains(linkKey(a, b));
}

void LoopbackNetwork::post(std::function<void()> fn)
{
    ++m_pending;
    QTimer::singleShot(0, [this, fn = std::move(fn)] {
        --m_pending;
        fn();
    });
}

void LoopbackNetwork::settle()
{
    while (m_pending > 0)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}
