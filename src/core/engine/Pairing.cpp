// SPDX-License-Identifier: MPL-2.0
#include "Pairing.h"
#include "store/Snapshot.h"

#include "Account.h"
#include "AccountManager.h"
#include "store/BlobStore.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"
#include "store/Schema.h"
#include "sync/IrohTransport.h"
#include "util/Log.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTimer>
#include <QUuid>
#include <QtEndian>

namespace e3 {

namespace {

constexpr qint64 kPiece = 1 << 20; // what one Get asks for
constexpr int kInFlight = 8;       // Gets outstanding at once
constexpr char kPrefix[] = "E3PAIR1:";

QByteArray json(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QByteArray sha256File(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return h.result().toHex();
}

// Compares in constant time: the token is a secret.
bool sameToken(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size() || a.isEmpty())
        return false;
    char diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        diff = char(diff | (a[i] ^ b[i]));
    return diff == 0;
}

QByteArray chunkHeader(bool blob, const QString &name, qint64 offset)
{
    QByteArray h(1 + 64 + 8, '\0');
    h[0] = blob ? 1 : 0;
    const QByteArray n = name.toLatin1().left(64);
    std::copy(n.cbegin(), n.cend(), h.begin() + 1);
    qToBigEndian(quint64(offset), h.data() + 65);
    return h;
}


// Whether an endpoint address offers only this machine's own addresses.
bool onlyLoopback(const QString &addr)
{
    const QJsonArray addrs = QJsonDocument::fromJson(addr.toUtf8()).object().value(QLatin1String("addrs")).toArray();
    for (const QJsonValue &a : addrs) {
        const QString ip = a.toObject().value(QLatin1String("Ip")).toString();
        if (!ip.startsWith(QLatin1String("127.")) && !ip.startsWith(QLatin1String("[::1]")))
            return false;
    }
    return !addrs.isEmpty();
}

} // namespace

namespace pairing {

QString formatCode(const Code &c)
{
    const QJsonObject o{{QStringLiteral("a"), c.addr},
                        {QStringLiteral("t"), QString::fromLatin1(c.token.toHex())},
                        {QStringLiteral("m"), c.account},
                        {QStringLiteral("r"), c.relay}};
    return QLatin1String(kPrefix) + QString::fromLatin1(json(o).toBase64(QByteArray::Base64UrlEncoding
                                                                          | QByteArray::OmitTrailingEquals));
}

std::optional<Code> parseCode(const QString &text)
{
    const QString t = text.trimmed();
    if (!t.startsWith(QLatin1String(kPrefix)))
        return std::nullopt;
    const QByteArray raw = QByteArray::fromBase64(t.mid(int(qstrlen(kPrefix))).toLatin1(),
                                                  QByteArray::Base64UrlEncoding);
    const QJsonObject o = QJsonDocument::fromJson(raw).object();
    Code c;
    c.addr = o.value(QLatin1String("a")).toString();
    c.token = QByteArray::fromHex(o.value(QLatin1String("t")).toString().toLatin1());
    c.account = o.value(QLatin1String("m")).toString();
    c.relay = o.value(QLatin1String("r")).toString();
    if (c.addr.isEmpty() || c.token.size() != 16 || !c.account.contains(u'@'))
        return std::nullopt;
    return c;
}

} // namespace pairing

// ---------------------------------------------------------------- offer

PairingOffer::PairingOffer(Account &account, QObject *parent) : QObject(parent), m_account(&account)
{
    if (!account.startDevices())
        return;
    m_transport = qobject_cast<sync::IrohTransport *>(account.deviceTransport());
    if (!m_transport)
        return;
    // So the code carries a relay; without one, the local addresses are
    // known at once.
    if (account.ctx().config.get(cfg::P2pRelay) != QLatin1String("off"))
        m_transport->waitOnline(3000);
    m_token.resize(16);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(m_token.data()), 4);
    m_expires = QDateTime::currentDateTimeUtc().addSecs(kValidMinutes * 60);
    m_code = pairing::formatCode({m_transport->addr(), m_token, account.addr(),
                                  account.ctx().config.get(cfg::P2pRelay)});
    m_transport->acceptAny(true);
    connect(m_transport, &sync::IrohTransport::strangerFrame, this, &PairingOffer::onFrame);
    connect(m_transport, &sync::IrohTransport::strangerClosed, this, &PairingOffer::onClosed);
    QTimer::singleShot(kValidMinutes * 60 * 1000, this, [this] {
        if (!m_used)
            fail(tr("The code expired."));
    });
}

PairingOffer::~PairingOffer()
{
    if (m_transport && m_account)
        m_transport->acceptAny(false);
}

void PairingOffer::fail(const QString &message)
{
    if (m_transport) {
        m_transport->acceptAny(false);
        if (m_conn)
            m_transport->sendFrame(m_conn, pairing::Denied, json({{QStringLiteral("reason"), message}}));
    }
    m_used = true;
    Q_EMIT finished(false, message);
}

void PairingOffer::onClosed(quint64 conn, const QString &)
{
    if (conn == m_conn && !m_device.isEmpty() && m_total && m_served < m_total)
        fail(tr("The other device went away."));
}

void PairingOffer::onFrame(quint64 conn, const QString &peer, quint8 kind, const QByteArray &data)
{
    const QJsonObject o = QJsonDocument::fromJson(data).object();
    if (kind == pairing::Request) {
        const QByteArray token = QByteArray::fromHex(o.value(QLatin1String("token")).toString().toLatin1());
        // One device per code, before it expires, holding the token.
        if (m_used || m_conn || !sameToken(token, m_token) || QDateTime::currentDateTimeUtc() > m_expires) {
            m_transport->sendFrame(conn, pairing::Denied, json({{QStringLiteral("reason"), QStringLiteral("refused")}}));
            m_transport->close(conn);
            return;
        }
        m_conn = conn;
        m_peer = peer;
        m_device = o.value(QLatin1String("device")).toString();
        m_requester = o.value(QLatin1String("name")).toString().left(64);
        if (m_requester.isEmpty())
            m_requester = tr("Another device");
        Q_EMIT requested(m_requester);
        return;
    }
    if (conn != m_conn)
        return;
    if (kind == pairing::Get) {
        serve(o);
    } else if (kind == pairing::Done) {
        // The new device has everything: from now on it is one of ours.
        auto &ctx = m_account->ctx();
        Transaction tx(ctx.db);
        ctx.db.run("INSERT INTO devices(id, name, paired_at, node_id) VALUES(?, ?, ?, ?) "
                   "ON CONFLICT(id) DO UPDATE SET name=excluded.name, node_id=excluded.node_id",
                   m_device, m_requester, QDateTime::currentMSecsSinceEpoch(), m_peer);
        ctx.ops.record(op::DevicePair, {{QStringLiteral("device"), m_device},
                                        {QStringLiteral("node"), m_peer},
                                        {QStringLiteral("name"), m_requester}});
        tx.commit();
        m_transport->acceptAny(false);
        m_transport->refreshPeers();
        m_used = true;
        m_account->notifyChanged();
        Q_EMIT finished(true, QString());
    } else if (kind == pairing::Denied) {
        fail(o.value(QLatin1String("reason")).toString());
    }
}

void PairingOffer::deny()
{
    fail(tr("Pairing was refused on this device."));
}

void PairingOffer::allow()
{
    if (!m_account || !m_conn || m_used || m_tmp)
        return;
    m_used = true;
    auto &ctx = m_account->ctx();
    // A consistent copy, without what belongs to this device alone.
    m_tmp = std::make_unique<QTemporaryDir>(m_account->dir() + QStringLiteral("/.pairing-XXXXXX"));
    m_dbPath = m_tmp->filePath(QStringLiteral("snapshot.db"));
    try {
        snapshotDatabase(ctx.db, m_dbPath);
    } catch (const std::exception &e) {
        fail(QString::fromUtf8(e.what()));
        return;
    }
    QFile db(m_dbPath);
    QJsonArray blobs;
    m_total = db.size();
    const QDir dir(ctx.blobs.dir());
    for (const QFileInfo &f : dir.entryInfoList(QDir::Files)) {
        if (f.fileName().size() != 64)
            continue;
        blobs.append(QJsonArray{f.fileName(), f.size()});
        m_total += f.size();
    }
    const Account::Settings s = m_account->settings();
    const QJsonObject manifest{
        {QStringLiteral("v"), 1},
        {QStringLiteral("account"), m_account->addr()},
        {QStringLiteral("device"), ctx.ops.deviceId()},
        {QStringLiteral("name"), ctx.config.get(cfg::DeviceName)},
        {QStringLiteral("node"), m_transport->nodeId()},
        {QStringLiteral("addr"), m_transport->addr()},
        {QStringLiteral("db"), QJsonObject{{QStringLiteral("size"), db.size()},
                                           {QStringLiteral("sha256"), QString::fromLatin1(sha256File(m_dbPath))}}},
        {QStringLiteral("blobs"), blobs},
        // The passwords travel only here, inside the authenticated channel.
        {QStringLiteral("in"), s.inPassword},
        {QStringLiteral("smtp"), s.smtpPassword},
        {QStringLiteral("oauth"), s.oauthRefresh}};
    m_transport->sendFrame(m_conn, pairing::Manifest, json(manifest));
}

void PairingOffer::serve(const QJsonObject &get)
{
    if (!m_tmp)
        return;
    const bool blob = get.value(QLatin1String("what")).toString() == QLatin1String("blob");
    const QString name = get.value(QLatin1String("name")).toString();
    const qint64 offset = qint64(get.value(QLatin1String("offset")).toDouble());
    QByteArray piece;
    if (blob) {
        if (name.size() != 64 || !m_account->ctx().blobs.exists(name))
            return fail(tr("A file asked for is not here."));
        if (name != m_cachedBlob) {
            m_cachedData = m_account->ctx().blobs.get(name);
            m_cachedBlob = name;
        }
        piece = m_cachedData.mid(offset, kPiece);
    } else {
        QFile f(m_dbPath);
        if (!f.open(QIODevice::ReadOnly) || !f.seek(offset))
            return fail(tr("The copy of the mailbox could not be read."));
        piece = f.read(kPiece);
    }
    m_served += piece.size();
    m_transport->sendFrame(m_conn, pairing::Chunk, chunkHeader(blob, name, offset) + piece);
    if (m_total)
        Q_EMIT progress(int(m_served * 100 / m_total));
}

// ---------------------------------------------------------------- join

PairingJoin::PairingJoin(AccountManager &manager, const QString &code, const QString &deviceName, QObject *parent)
    : QObject(parent), m_manager(manager), m_name(deviceName.trimmed().left(64))
{
    const auto c = pairing::parseCode(code);
    if (!c)
        return;
    m_code = *c;
    m_valid = true;
    for (Account *a : manager.accounts()) {
        if (a->addr().compare(m_code.account, Qt::CaseInsensitive) == 0) {
            m_valid = false; // this account is already on this device
            return;
        }
    }
    m_secret = sync::IrohTransport::generateSecret();
    m_device = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_work = std::make_unique<QTemporaryDir>(manager.root() + QStringLiteral("/.join-XXXXXX"));
    m_scratchDb = std::make_unique<Database>(m_work->filePath(QStringLiteral("scratch.db")));
    Schema::migrate(*m_scratchDb);
    sync::IrohTransport::Options o;
    o.secret = m_secret;
    if (m_code.relay == QLatin1String("off")) {
        o.relay = sync::IrohTransport::Relay::Off;
        // An offer reachable only on this machine is answered from here too.
        o.loopback = onlyLoopback(m_code.addr);
    } else if (m_code.relay.startsWith(QLatin1String("http"))) {
        o.relay = sync::IrohTransport::Relay::Custom;
        o.relayUrl = m_code.relay;
    }
    m_transport = std::make_unique<sync::IrohTransport>(*m_scratchDb, m_device, o);
    connect(m_transport.get(), &sync::IrohTransport::strangerConnected, this, &PairingJoin::onConnected);
    connect(m_transport.get(), &sync::IrohTransport::strangerFrame, this, &PairingJoin::onFrame);
    connect(m_transport.get(), &sync::IrohTransport::strangerClosed, this, &PairingJoin::onClosed);
    QMetaObject::invokeMethod(this, [this] {
        m_conn = m_transport->dial(m_code.addr);
        if (!m_conn)
            fail(tr("The code could not be read."));
    }, Qt::QueuedConnection);
}

PairingJoin::~PairingJoin() = default;

void PairingJoin::fail(const QString &message)
{
    if (m_finished)
        return;
    m_finished = true;
    m_db.close();
    // Usually called from within the transport's own poll: it goes once
    // that has returned.
    if (m_transport)
        m_transport.release()->deleteLater();
    Q_EMIT finished(false, message, 0);
}

void PairingJoin::onConnected(quint64 conn, const QString &, bool)
{
    if (conn != m_conn)
        return;
    m_transport->sendFrame(conn, pairing::Request, json({{QStringLiteral("token"), QString::fromLatin1(m_code.token.toHex())},
                                                         {QStringLiteral("name"), m_name},
                                                         {QStringLiteral("device"), m_device}}));
    Q_EMIT waiting();
}

void PairingJoin::onClosed(quint64 conn, const QString &reason)
{
    if (conn == m_conn && !m_finished)
        fail(tr("The connection to the other device was lost: %1").arg(reason));
}

void PairingJoin::onFrame(quint64 conn, const QString &, quint8 kind, const QByteArray &data)
{
    if (conn != m_conn || m_finished)
        return;
    if (kind == pairing::Denied)
        fail(tr("The other device did not allow it."));
    else if (kind == pairing::Manifest)
        onManifest(data);
    else if (kind == pairing::Chunk)
        onChunk(data);
}

void PairingJoin::onManifest(const QByteArray &data)
{
    m_manifest = QJsonDocument::fromJson(data).object();
    if (m_manifest.value(QLatin1String("account")).toString().compare(m_code.account, Qt::CaseInsensitive) != 0)
        return fail(tr("The other device offered a different account."));
    m_stage = m_work->filePath(QStringLiteral("account"));
    QDir().mkpath(m_stage + QStringLiteral("/blobs"));
    m_db.setFileName(m_stage + QStringLiteral("/mail.db"));
    if (!m_db.open(QIODevice::WriteOnly))
        return fail(tr("Cannot write here."));
    const QJsonObject db = m_manifest.value(QLatin1String("db")).toObject();
    const qint64 dbSize = qint64(db.value(QLatin1String("size")).toDouble());
    m_dbSha = db.value(QLatin1String("sha256")).toString();
    m_total = dbSize;
    for (qint64 off = 0; off < dbSize; off += kPiece)
        m_toAsk.append({false, QString(), off, dbSize});
    for (const QJsonValue &v : m_manifest.value(QLatin1String("blobs")).toArray()) {
        const QString name = v.toArray().at(0).toString();
        const qint64 size = qint64(v.toArray().at(1).toDouble());
        if (name.size() != 64)
            continue;
        m_total += size;
        for (qint64 off = 0; off < qMax<qint64>(size, 1); off += kPiece)
            m_toAsk.append({true, name, off, size});
    }
    requestMore();
}

void PairingJoin::requestMore()
{
    while (m_asked.size() < kInFlight && !m_toAsk.isEmpty()) {
        const Piece p = m_toAsk.takeFirst();
        m_transport->sendFrame(m_conn, pairing::Get,
                               json({{QStringLiteral("what"), p.blob ? QStringLiteral("blob") : QStringLiteral("db")},
                                     {QStringLiteral("name"), p.name},
                                     {QStringLiteral("offset"), double(p.offset)}}));
        m_asked.append(p);
    }
    if (m_asked.isEmpty() && m_toAsk.isEmpty())
        complete();
}

void PairingJoin::onChunk(const QByteArray &data)
{
    if (m_asked.isEmpty() || data.size() < 73)
        return fail(tr("The other device sent something unexpected."));
    const Piece p = m_asked.takeFirst();
    const bool blob = data[0] == 1;
    const QString name = QString::fromLatin1(data.mid(1, 64)).remove(QChar(0));
    const qint64 offset = qint64(qFromBigEndian<quint64>(data.constData() + 65));
    const QByteArray bytes = data.mid(73);
    if (blob != p.blob || offset != p.offset || (blob && name != p.name))
        return fail(tr("The other device sent something unexpected."));
    m_received += bytes.size();
    if (!blob) {
        if (!m_db.seek(offset) || m_db.write(bytes) != bytes.size())
            return fail(tr("Cannot write here."));
    } else {
        if (offset == 0)
            m_blobData.clear();
        m_blobData += bytes;
        if (m_blobData.size() >= p.size) {
            // Content-addressed: a blob whose bytes do not hash to its name
            // was changed on the way, or is not what it claims to be.
            BlobStore store(m_stage + QStringLiteral("/blobs"));
            if (store.put(m_blobData) != p.name)
                return fail(tr("A file arrived damaged."));
            m_blobData.clear();
        }
    }
    if (m_total)
        Q_EMIT progress(int(m_received * 100 / m_total));
    requestMore();
}

void PairingJoin::complete()
{
    if (m_finished)
        return;
    m_db.close();
    if (QString::fromLatin1(sha256File(m_db.fileName())) != m_dbSha)
        return fail(tr("The copy of the mailbox arrived damaged."));
    const QString offerer = m_manifest.value(QLatin1String("device")).toString();
    try {
        // This device: an id and an endpoint of its own, and the device it
        // came from as its first peer.
        Database db(m_db.fileName());
        Schema::migrate(db);
        Config c(db);
        c.set(cfg::DeviceId, m_device);
        c.set(cfg::P2pSecret, QString::fromLatin1(m_secret.toHex()));
        c.set(cfg::DeviceName, m_name);
        if (!m_code.relay.isEmpty())
            c.set(cfg::P2pRelay, m_code.relay);
        db.run("INSERT OR REPLACE INTO devices(id, name, paired_at, node_id, addr) VALUES(?, ?, ?, ?, ?)", offerer,
               m_manifest.value(QLatin1String("name")).toString(), QDateTime::currentMSecsSinceEpoch(),
               m_manifest.value(QLatin1String("node")).toString(), m_manifest.value(QLatin1String("addr")).toString());
    } catch (const std::exception &e) {
        return fail(QString::fromUtf8(e.what()));
    }
    m_transport->sendFrame(m_conn, pairing::Done, QByteArray());
    // The same key is this account's endpoint from now on; let this one go
    // before the account starts its own.
    QTimer::singleShot(300, this, [this] {
        m_transport.reset();
        m_scratchDb.reset();
        Account *a = m_manager.adopt(m_stage);
        if (!a)
            return fail(tr("Cannot write here."));
        a->storePasswords(m_manifest.value(QLatin1String("in")).toString(),
                          m_manifest.value(QLatin1String("smtp")).toString());
        a->storeRefreshToken(m_manifest.value(QLatin1String("oauth")).toString());
        // Every server message the copy holds is here now: say so, or the
        // other devices would keep it on the server forever.
        auto &ctx = a->ctx();
        Transaction tx(ctx.db);
        Statement st(ctx.db, "SELECT remote_key FROM server_msgs WHERE msg_id IS NOT NULL AND deleted=0");
        while (st.step())
            ctx.ops.record(op::ServerAck, {{QStringLiteral("key"), st.text(0)}});
        tx.commit();
        m_finished = true;
        Q_EMIT finished(true, QString(), a->id());
    });
}

} // namespace e3
