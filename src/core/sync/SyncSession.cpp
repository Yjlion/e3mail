// SPDX-License-Identifier: MPL-2.0
#include "SyncSession.h"

#include "DeviceTransport.h"
#include "mail/Ingest.h"
#include "store/BlobStore.h"
#include "store/Database.h"
#include "util/Log.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>

namespace e3::sync {

namespace {

constexpr int kWireVersion = 1;

// The clock goes as a string: it uses 64 bits, and JSON numbers are doubles.
QJsonObject toWire(const Op &o)
{
    return {{QStringLiteral("d"), o.device},
            {QStringLiteral("s"), o.seq},
            {QStringLiteral("h"), QString::number(o.hlc)},
            {QStringLiteral("k"), o.kind},
            {QStringLiteral("p"), o.payload}};
}

Op fromWire(const QJsonObject &w)
{
    Op o;
    o.device = w.value(QLatin1String("d")).toString();
    o.seq = qint64(w.value(QLatin1String("s")).toDouble());
    o.hlc = w.value(QLatin1String("h")).toString().toLongLong();
    o.kind = w.value(QLatin1String("k")).toString();
    o.payload = w.value(QLatin1String("p")).toObject();
    return o;
}

QByteArray message(const QString &type, QJsonObject body)
{
    body.insert(QStringLiteral("v"), kWireVersion);
    body.insert(QStringLiteral("t"), type);
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

} // namespace

SyncSession::SyncSession(MailContext &ctx, DeviceTransport &transport, QObject *parent)
    : QObject(parent), m_ctx(ctx), m_transport(transport)
{
    connect(&transport, &DeviceTransport::connected, this, &SyncSession::onConnected);
    connect(&transport, &DeviceTransport::disconnected, this, &SyncSession::onDisconnected);
    connect(&transport, &DeviceTransport::received, this, &SyncSession::onReceived);
    connect(&transport, &DeviceTransport::blobRequested, this, &SyncSession::onBlobRequested);
    connect(&transport, &DeviceTransport::blobReceived, this, &SyncSession::onBlobReceived);
}

void SyncSession::pair(MailContext &ctx, const QString &device, const QString &name)
{
    ctx.db.run("INSERT OR IGNORE INTO devices(id, name, paired_at) VALUES(?, ?, ?)", device, name,
               QDateTime::currentMSecsSinceEpoch());
}

void SyncSession::unpair(MailContext &ctx, const QString &device)
{
    ctx.db.run("DELETE FROM devices WHERE id=?", device);
}

bool SyncSession::isPaired(MailContext &ctx, const QString &device)
{
    return ctx.db.queryInt("SELECT 1 FROM devices WHERE id=?", device).has_value();
}

void SyncSession::push()
{
    for (const QString &p : std::as_const(m_peers))
        sendMissing(p);
}

void SyncSession::onConnected(const QString &device)
{
    if (!isPaired(m_ctx, device)) {
        qCWarning(lcSync) << "ignoring a device that is not paired:" << device;
        return;
    }
    m_peers.insert(device);
    m_peerHave.remove(device);
    sendHello(device);
}

void SyncSession::onDisconnected(const QString &device)
{
    m_peers.remove(device);
    m_peerHave.remove(device);
}

void SyncSession::sendHello(const QString &device)
{
    QJsonObject have;
    const OpApply::Versions mine = OpApply::versions(m_ctx.db);
    for (auto it = mine.cbegin(); it != mine.cend(); ++it)
        have.insert(it.key(), it.value());
    m_transport.send(device, message(QStringLiteral("hello"), {{QStringLiteral("have"), have}}));
}

void SyncSession::sendMissing(const QString &device)
{
    // Until the peer has said what it has, sending would only guess.
    if (!m_peerHave.contains(device))
        return;
    OpApply::Versions &have = m_peerHave[device];
    for (;;) {
        const QList<Op> batch = OpApply::missing(m_ctx.db, have, kBatch);
        if (batch.isEmpty())
            return;
        QJsonArray ops;
        for (const Op &o : batch) {
            ops.append(toWire(o));
            have.insert(o.device, qMax(have.value(o.device), o.seq));
        }
        m_transport.send(device, message(QStringLiteral("ops"), {{QStringLiteral("ops"), ops}}));
    }
}

void SyncSession::onReceived(const QString &device, const QByteArray &bytes)
{
    if (!m_peers.contains(device))
        return;
    const QJsonObject msg = QJsonDocument::fromJson(bytes).object();
    if (msg.value(QLatin1String("v")).toInt() != kWireVersion) {
        qCWarning(lcSync) << "ignoring a message of unknown version from" << device;
        return;
    }
    const QString type = msg.value(QLatin1String("t")).toString();
    try {
        if (type == QLatin1String("hello")) {
            OpApply::Versions have;
            const QJsonObject h = msg.value(QLatin1String("have")).toObject();
            for (auto it = h.begin(); it != h.end(); ++it)
                have.insert(it.key(), qint64(it.value().toDouble()));
            m_peerHave.insert(device, have);
            sendMissing(device);
        } else if (type == QLatin1String("ops")) {
            QList<Op> ops;
            for (const QJsonValue &v : msg.value(QLatin1String("ops")).toArray())
                ops.append(fromWire(v.toObject()));
            handleOps(device, ops);
        }
    } catch (const std::exception &e) {
        qCWarning(lcSync) << "sync with" << device << "failed:" << e.what();
    }
}

void SyncSession::handleOps(const QString &device, const QList<Op> &ops)
{
    const OpApply::Result r = OpApply::apply(m_ctx, ops);
    m_ctx.db.run("UPDATE devices SET last_seen=? WHERE id=?", QDateTime::currentMSecsSinceEpoch(), device);
    // The sender has what it sent.
    if (m_peerHave.contains(device)) {
        OpApply::Versions &have = m_peerHave[device];
        for (const Op &o : ops)
            have.insert(o.device, qMax(have.value(o.device), o.seq));
    }
    bool changed = r.changed;
    for (const QString &name : r.wantedRaw) {
        if (m_ctx.blobs.exists(name)) {
            changed |= !mail::Ingest::process(m_ctx, m_ctx.blobs.get(name)).duplicate;
        } else {
            m_transport.requestBlob(device, name);
        }
    }
    if (r.gap)
        sendHello(device); // ask again from where we are
    if (r.accepted) {
        // Pass news on to the other devices.
        for (const QString &p : std::as_const(m_peers)) {
            if (p != device)
                sendMissing(p);
        }
    }
    if (changed)
        Q_EMIT this->changed();
}

void SyncSession::onBlobRequested(const QString &device, const QString &name)
{
    if (!m_peers.contains(device))
        return;
    m_transport.provideBlob(device, name, m_ctx.blobs.exists(name) ? m_ctx.blobs.get(name) : QByteArray());
}

void SyncSession::onBlobReceived(const QString &device, const QString &name, const QByteArray &data)
{
    if (!m_peers.contains(device))
        return;
    if (QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()) != name) {
        qCWarning(lcSync) << "discarding a blob that does not match its name from" << device;
        return;
    }
    try {
        if (!mail::Ingest::process(m_ctx, data).duplicate)
            Q_EMIT changed();
    } catch (const std::exception &e) {
        qCWarning(lcSync) << "cannot store a message from" << device << e.what();
    }
}

} // namespace e3::sync
