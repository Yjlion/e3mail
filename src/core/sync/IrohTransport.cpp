// SPDX-License-Identifier: MPL-2.0
#include "IrohTransport.h"

#include "e3p2p.h"
#include "store/Database.h"
#include "util/Log.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace e3::sync {

namespace {
constexpr qsizetype kNameLen = 64; // blob names are SHA-256 in hex
}

IrohTransport::IrohTransport(Database &db, const QString &self, const Options &options, QObject *parent)
    : DeviceTransport(parent), m_db(db), m_self(self)
{
    if (options.secret.size() != 32)
        return;
    const int relay = options.relay == Relay::Off      ? E3P2P_RELAY_OFF
                    : options.relay == Relay::Custom ? E3P2P_RELAY_CUSTOM
                                                     : E3P2P_RELAY_DEFAULT;
    const QByteArray url = options.relayUrl.toUtf8();
    m_node = e3p2p_start(reinterpret_cast<const uint8_t *>(options.secret.constData()), relay,
                         url.isEmpty() ? nullptr : url.constData(), options.port, options.loopback ? 1 : 0,
                         &IrohTransport::wake, this);
    if (!m_node) {
        qCWarning(lcSync) << "cannot start the device endpoint";
        return;
    }
    refreshPeers();
}

IrohTransport::~IrohTransport()
{
    // No more wakes once this returns; queued polls die with the object.
    if (m_node)
        e3p2p_stop(m_node);
}

QByteArray IrohTransport::generateSecret()
{
    QByteArray s(32, '\0');
    e3p2p_generate_secret(reinterpret_cast<uint8_t *>(s.data()));
    return s;
}

QString IrohTransport::nodeId() const
{
    if (!m_node)
        return {};
    char id[65] = {};
    e3p2p_node_id(m_node, id);
    return QString::fromLatin1(id);
}

QString IrohTransport::addr() const
{
    if (!m_node)
        return {};
    char *a = e3p2p_addr(m_node);
    const QString out = QString::fromUtf8(a);
    e3p2p_string_free(a);
    return out;
}

bool IrohTransport::waitOnline(int ms)
{
    return m_node && e3p2p_wait_online(m_node, quint64(ms)) == 1;
}

void IrohTransport::refreshPeers()
{
    if (!m_node)
        return;
    QSet<QString> now;
    Statement st(m_db, "SELECT node_id FROM devices WHERE node_id IS NOT NULL");
    while (st.step())
        now.insert(st.text(0));
    for (const QString &n : now - m_allowed)
        e3p2p_allow(m_node, n.toLatin1().constData(), 1);
    for (const QString &gone : m_allowed - now) {
        e3p2p_allow(m_node, gone.toLatin1().constData(), 0);
        for (auto it = m_conns.cbegin(); it != m_conns.cend(); ++it) {
            if (it->peer == gone)
                e3p2p_close(m_node, it.key());
        }
    }
    m_allowed = now;
}

void IrohTransport::acceptAny(bool on)
{
    if (m_node)
        e3p2p_accept_any(m_node, on ? 1 : 0);
}

void IrohTransport::dialPaired()
{
    if (!m_node)
        return;
    QSet<QString> busy;
    for (const quint64 c : std::as_const(m_dialing))
        busy.insert(m_conns.value(c).peer);
    Statement st(m_db, "SELECT id, node_id, addr FROM devices WHERE node_id IS NOT NULL");
    while (st.step()) {
        const QString device = st.text(0);
        const QString node = st.text(1);
        if (m_byDevice.contains(device) || busy.contains(node))
            continue;
        // The address it was last reached at; without one, its id alone is
        // looked up through the relay's address service.
        const QString addr = st.isNull(2) || st.text(2).isEmpty() ? node : st.text(2);
        if (const quint64 c = dial(addr)) {
            m_conns[c].peer = node;
            m_dialing.insert(c);
        }
    }
}

quint64 IrohTransport::dial(const QString &addr)
{
    if (!m_node)
        return 0;
    return e3p2p_connect(m_node, addr.toUtf8().constData());
}

void IrohTransport::wake(void *self)
{
    auto *t = static_cast<IrohTransport *>(self);
    // One poll drains everything waiting; there is no need to queue more.
    if (!t->m_wakePending.exchange(true))
        QMetaObject::invokeMethod(t, &IrohTransport::poll, Qt::QueuedConnection);
}

void IrohTransport::poll()
{
    m_wakePending = false;
    if (!m_node)
        return;
    e3p2p_event e{};
    while (e3p2p_poll(m_node, &e) == 1) {
        const quint64 conn = e.conn;
        const QString peer = QString::fromLatin1(e.peer);
        const QByteArray data(reinterpret_cast<const char *>(e.data), qsizetype(e.len));
        const int kind = e.kind;
        const quint8 frame = e.frame;
        const bool incoming = e.incoming != 0;
        e3p2p_event_free(&e);
        switch (kind) {
        case E3P2P_EVENT_CONNECTED: {
            m_dialing.remove(conn);
            Conn &c = m_conns[conn];
            c.peer = peer;
            c.incoming = incoming;
            // Say who we are; the device is known once the other side has too.
            // with where we can be reached now, for the next time.
            sendFrame(conn, Hello, QJsonDocument(QJsonObject{{QStringLiteral("device"), m_self},
                                                             {QStringLiteral("addr"), addr()}})
                                       .toJson(QJsonDocument::Compact));
            Q_EMIT strangerConnected(conn, peer, incoming);
            break;
        }
        case E3P2P_EVENT_FRAME:
            onFrame(conn, frame, data);
            break;
        case E3P2P_EVENT_CLOSED:
            onClosed(conn, QString::fromUtf8(data));
            break;
        default:
            break;
        }
    }
}

void IrohTransport::onHello(quint64 conn, const QByteArray &data)
{
    Conn &c = m_conns[conn];
    const QString device = QJsonDocument::fromJson(data).object().value(QLatin1String("device")).toString();
    // The device must be paired, and be the one this endpoint belongs to.
    const auto node = m_db.queryText("SELECT node_id FROM devices WHERE id=?", device);
    if (!node || *node != c.peer)
        return; // not a paired device: a stranger, for pairing to decide about
    if (const auto existing = m_byDevice.constFind(device); existing != m_byDevice.cend()) {
        // Both dialed at once. Both sides keep the connection dialed by the
        // endpoint with the smaller id, so they agree without talking.
        const Conn &old = m_conns.value(*existing);
        const auto dialer = [this](const Conn &x) { return x.incoming ? x.peer : nodeId(); };
        if (dialer(old) <= dialer(c)) {
            e3p2p_close(m_node, conn);
            return;
        }
        const quint64 oldConn = *existing;
        m_conns[oldConn].device.clear();
        e3p2p_close(m_node, oldConn);
        c.device = device;
        m_byDevice.insert(device, conn);
        return; // still connected: SyncSession goes on over the new one
    }
    c.device = device;
    m_byDevice.insert(device, conn);
    const QString addr = QJsonDocument::fromJson(data).object().value(QLatin1String("addr")).toString();
    if (!addr.isEmpty())
        m_db.run("UPDATE devices SET addr=? WHERE id=?", addr, device);
    qCInfo(lcSync) << "device connected:" << device;
    Q_EMIT connected(device);
}

void IrohTransport::onFrame(quint64 conn, quint8 kind, const QByteArray &data)
{
    const QString device = m_conns.value(conn).device;
    if (kind == Hello) {
        onHello(conn, data);
        return;
    }
    if (device.isEmpty()) {
        if (kind >= 16)
            Q_EMIT strangerFrame(conn, m_conns.value(conn).peer, kind, data);
        return;
    }
    switch (kind) {
    case Sync:
        Q_EMIT received(device, data);
        break;
    case BlobRequest:
        Q_EMIT blobRequested(device, QString::fromLatin1(data));
        break;
    case Blob:
        if (data.size() >= kNameLen)
            Q_EMIT blobReceived(device, QString::fromLatin1(data.left(kNameLen)), data.mid(kNameLen));
        break;
    case BlobMissing:
        Q_EMIT blobMissing(device, QString::fromLatin1(data));
        break;
    default:
        break;
    }
}

void IrohTransport::onClosed(quint64 conn, const QString &why)
{
    m_dialing.remove(conn);
    const Conn c = m_conns.take(conn);
    if (!c.device.isEmpty() && m_byDevice.value(c.device) == conn) {
        m_byDevice.remove(c.device);
        qCInfo(lcSync) << "device disconnected:" << c.device << why;
        Q_EMIT disconnected(c.device);
    } else if (c.device.isEmpty()) {
        Q_EMIT strangerClosed(conn, why);
    }
}

bool IrohTransport::sendTo(const QString &device, quint8 kind, const QByteArray &data)
{
    const auto it = m_byDevice.constFind(device);
    if (it == m_byDevice.cend() || !m_node)
        return false;
    return e3p2p_send(m_node, *it, kind, reinterpret_cast<const uint8_t *>(data.constData()), size_t(data.size())) == 1;
}

void IrohTransport::send(const QString &device, const QByteArray &message)
{
    sendTo(device, Sync, message);
}

void IrohTransport::requestBlob(const QString &device, const QString &name)
{
    sendTo(device, BlobRequest, name.toLatin1());
}

void IrohTransport::provideBlob(const QString &device, const QString &name, const QByteArray &data)
{
    if (data.isEmpty() || data.size() + kNameLen > qsizetype(E3P2P_MAX_FRAME))
        sendTo(device, BlobMissing, name.toLatin1());
    else
        sendTo(device, Blob, name.toLatin1().leftJustified(kNameLen, '\0', true) + data);
}

void IrohTransport::sendFrame(quint64 conn, quint8 kind, const QByteArray &data)
{
    if (m_node)
        e3p2p_send(m_node, conn, kind, reinterpret_cast<const uint8_t *>(data.constData()), size_t(data.size()));
}

void IrohTransport::close(quint64 conn)
{
    if (m_node)
        e3p2p_close(m_node, conn);
}

} // namespace e3::sync
