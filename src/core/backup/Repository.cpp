// SPDX-License-Identifier: MPL-2.0
#include "Repository.h"

#include "store/BlobStore.h"
#include "store/Database.h"
#include "store/Snapshot.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QTimeZone>

#include <algorithm>

namespace e3::backup {

namespace {

constexpr qint64 kChunk = 8 << 20; // the database goes up in pieces of 8 MiB
const QByteArray kKeyAd = "e3backup key v1";

QByteArray sha256(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

QByteArray toJson(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QString timeTag(const QDateTime &t)
{
    return t.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmsszzz'Z'"));
}

} // namespace

bool Repository::exists()
{
    return m_dest.get(QStringLiteral("key")).has_value();
}

QByteArray Repository::create(const QByteArray &passphrase, const crypto::KdfParams &given)
{
    crypto::KdfParams p = given;
    if (p.salt.isEmpty())
        p.salt = crypto::random(16);
    const QByteArray master = crypto::random(32);
    const QByteArray kek = crypto::argon2id(passphrase, p);
    const QJsonObject key{
        {QStringLiteral("v"), 1},
        {QStringLiteral("kdf"), QJsonObject{{QStringLiteral("alg"), QStringLiteral("argon2id")},
                                            {QStringLiteral("m"), qint64(p.memoryKiB)},
                                            {QStringLiteral("t"), qint64(p.iterations)},
                                            {QStringLiteral("p"), qint64(p.parallelism)},
                                            {QStringLiteral("salt"), QString::fromLatin1(p.salt.toBase64())}}},
        {QStringLiteral("key"), QString::fromLatin1(crypto::seal(kek, master, kKeyAd).toBase64())}};
    m_dest.prepare();
    m_dest.put(QStringLiteral("key"), toJson(key));
    setKey(master);
    return master;
}

QByteArray Repository::unlock(const QByteArray &passphrase)
{
    const auto raw = m_dest.get(QStringLiteral("key"));
    if (!raw)
        throw Damaged("there is no backup there");
    const QJsonObject o = QJsonDocument::fromJson(*raw).object();
    const QJsonObject k = o.value(QLatin1String("kdf")).toObject();
    if (o.value(QLatin1String("v")).toInt() != 1 || k.value(QLatin1String("alg")).toString() != QLatin1String("argon2id"))
        throw Damaged("a backup this version of e3mail cannot read");
    crypto::KdfParams p;
    p.memoryKiB = quint32(k.value(QLatin1String("m")).toInteger());
    p.iterations = quint32(k.value(QLatin1String("t")).toInteger());
    p.parallelism = quint32(k.value(QLatin1String("p")).toInteger());
    p.salt = QByteArray::fromBase64(k.value(QLatin1String("salt")).toString().toLatin1());
    // Bounds, so a planted key file cannot make this device spend gigabytes.
    if (p.memoryKiB < 8 || p.memoryKiB > 1024 * 1024 || p.iterations < 1 || p.iterations > 20 || p.parallelism < 1
        || p.parallelism > 8)
        throw Damaged("a backup key with unusual parameters");
    const auto master = crypto::open(crypto::argon2id(passphrase, p),
                                     QByteArray::fromBase64(o.value(QLatin1String("key")).toString().toLatin1()), kKeyAd);
    if (!master || master->size() != 32)
        throw BadPassphrase();
    setKey(*master);
    return *master;
}

void Repository::setKey(const QByteArray &master)
{
    m_master = master;
    m_kName = crypto::hmac(master, "e3backup names");
    m_kObject = crypto::hmac(master, "e3backup objects");
    m_kSnapshot = crypto::hmac(master, "e3backup snapshots");
}

QString Repository::nameKeyed(const QByteArray &label, const QByteArray &data) const
{
    return QString::fromLatin1(crypto::hmac(m_kName, label + data).toHex());
}

QString Repository::objectName(const QByteArray &sha256hex) const
{
    return QStringLiteral("objects/") + nameKeyed("object:", sha256hex);
}

QString Repository::deviceTag(const QString &device) const
{
    return nameKeyed("device:", device.toUtf8()).left(16);
}

void Repository::putObject(const QString &name, const QByteArray &plain)
{
    m_dest.put(name, crypto::seal(m_kObject, qCompress(plain, 6), name.toUtf8()));
}

QByteArray Repository::getObject(const QString &name)
{
    const auto sealed = m_dest.get(name);
    if (!sealed)
        throw Damaged(QStringLiteral("%1 is missing").arg(name).toStdString());
    const auto plain = crypto::open(m_kObject, *sealed, name.toUtf8());
    if (!plain)
        throw Damaged(QStringLiteral("%1 does not decrypt").arg(name).toStdString());
    return qUncompress(*plain);
}

Repository::Result Repository::backup(Database &db, const QByteArray &dbKey, const BlobStore &blobs,
                                      const Contents &about)
{
    if (m_master.isEmpty())
        throw Damaged("no key");
    Result r;
    QSet<QString> there;
    for (const Entry &e : m_dest.list(QStringLiteral("objects")))
        there.insert(QStringLiteral("objects/") + e.name);
    auto upload = [&](const QByteArray &plain) {
        const QString name = objectName(sha256(plain));
        if (there.contains(name)) {
            ++r.reused;
            return name;
        }
        putObject(name, plain);
        there.insert(name);
        ++r.uploaded;
        r.bytes += plain.size();
        return name;
    };

    Contents c = about;
    if (!c.created.isValid()) // tests date them
        c.created = QDateTime::currentDateTimeUtc();
    // The database, from a consistent copy without this device's secrets.
    QTemporaryDir tmp;
    const QString copy = tmp.filePath(QStringLiteral("mail.db"));
    const QByteArray snapshotKey = crypto::random(32);
    snapshotDatabase(db, copy, dbKey, snapshotKey);
    c.secrets.insert(QStringLiteral("dbkey"), QString::fromLatin1(snapshotKey.toBase64()));
    QFile f(copy);
    if (!f.open(QIODevice::ReadOnly))
        throw Damaged("cannot read the copy of the database");
    QCryptographicHash whole(QCryptographicHash::Sha256);
    c.dbSize = f.size();
    while (!f.atEnd()) {
        const QByteArray piece = f.read(kChunk);
        whole.addData(piece);
        c.dbChunks.append(upload(piece));
    }
    c.dbSha256 = QString::fromLatin1(whole.result().toHex());
    c.schema = int(db.queryInt("PRAGMA user_version").value_or(0));

    // Every blob the database refers to, by the name it already has (its
    // SHA-256), opened and checked through the store.
    for (const QString &sha : BlobStore::referenced(db)) {
        const QString name = objectName(sha.toLatin1());
        if (!blobs.exists(sha))
            continue; // collected meanwhile, or never fetched here
        c.blobs.append(sha);
        if (there.contains(name)) {
            ++r.reused;
            continue;
        }
        const QByteArray data = blobs.get(sha);
        if (data.isEmpty() || sha256(data) != sha.toLatin1()) {
            c.blobs.removeLast();
            continue; // damaged here: never back it up as if it were fine
        }
        putObject(name, data);
        there.insert(name);
        ++r.uploaded;
        r.bytes += data.size();
    }

    QJsonArray chunks, blobNames;
    for (const QString &n : std::as_const(c.dbChunks))
        chunks.append(n);
    for (const QString &n : std::as_const(c.blobs))
        blobNames.append(n);
    const QJsonObject manifest{{QStringLiteral("v"), 1},
                               {QStringLiteral("account"), c.account},
                               {QStringLiteral("device"), c.device},
                               {QStringLiteral("deviceName"), c.deviceName},
                               {QStringLiteral("created"), c.created.toString(Qt::ISODate)},
                               {QStringLiteral("schema"), c.schema},
                               {QStringLiteral("db"), QJsonObject{{QStringLiteral("chunks"), chunks},
                                                                  {QStringLiteral("sha256"), c.dbSha256},
                                                                  {QStringLiteral("size"), c.dbSize}}},
                               {QStringLiteral("blobs"), blobNames},
                               {QStringLiteral("secrets"), c.secrets}};
    r.snapshot = QStringLiteral("snapshots/%1-%2").arg(deviceTag(c.device), timeTag(c.created));
    m_dest.put(r.snapshot, crypto::seal(m_kSnapshot, qCompress(toJson(manifest), 6), r.snapshot.toUtf8()));
    return r;
}

QList<Snapshot> Repository::snapshots()
{
    QList<Snapshot> out;
    for (const Entry &e : m_dest.list(QStringLiteral("snapshots"))) {
        const qsizetype dash = e.name.indexOf(u'-');
        if (dash < 0)
            continue;
        Snapshot s;
        s.name = QStringLiteral("snapshots/") + e.name;
        s.device = e.name.left(dash);
        s.created = QDateTime::fromString(e.name.mid(dash + 1), QStringLiteral("yyyyMMdd'T'HHmmsszzz'Z'"));
        s.created.setTimeZone(QTimeZone::UTC);
        if (s.created.isValid())
            out.append(s);
    }
    std::sort(out.begin(), out.end(), [](const Snapshot &a, const Snapshot &b) { return a.created > b.created; });
    return out;
}

Contents Repository::contents(const QString &snapshot)
{
    const auto sealed = m_dest.get(snapshot);
    if (!sealed)
        throw Damaged(QStringLiteral("%1 is missing").arg(snapshot).toStdString());
    const auto plain = crypto::open(m_kSnapshot, *sealed, snapshot.toUtf8());
    if (!plain)
        throw Damaged(QStringLiteral("%1 does not decrypt").arg(snapshot).toStdString());
    const QJsonObject o = QJsonDocument::fromJson(qUncompress(*plain)).object();
    if (o.value(QLatin1String("v")).toInt() != 1)
        throw Damaged("a snapshot this version of e3mail cannot read");
    Contents c;
    c.account = o.value(QLatin1String("account")).toString();
    c.device = o.value(QLatin1String("device")).toString();
    c.deviceName = o.value(QLatin1String("deviceName")).toString();
    c.created = QDateTime::fromString(o.value(QLatin1String("created")).toString(), Qt::ISODate);
    c.schema = o.value(QLatin1String("schema")).toInt();
    const QJsonObject db = o.value(QLatin1String("db")).toObject();
    for (const QJsonValue &v : db.value(QLatin1String("chunks")).toArray())
        c.dbChunks.append(v.toString());
    c.dbSha256 = db.value(QLatin1String("sha256")).toString();
    c.dbSize = db.value(QLatin1String("size")).toInteger();
    for (const QJsonValue &v : o.value(QLatin1String("blobs")).toArray())
        c.blobs.append(v.toString());
    c.secrets = o.value(QLatin1String("secrets")).toObject();
    return c;
}

void Repository::verify(const QString &snapshot, int sample)
{
    const Contents c = contents(snapshot);
    QSet<QString> there;
    for (const Entry &e : m_dest.list(QStringLiteral("objects")))
        there.insert(QStringLiteral("objects/") + e.name);
    QCryptographicHash whole(QCryptographicHash::Sha256);
    for (const QString &n : c.dbChunks) {
        const QByteArray piece = getObject(n);
        if (objectName(sha256(piece)) != n)
            throw Damaged(QStringLiteral("%1 holds something else").arg(n).toStdString());
        whole.addData(piece);
    }
    if (QString::fromLatin1(whole.result().toHex()) != c.dbSha256)
        throw Damaged("the database does not match its manifest");
    for (const QString &sha : c.blobs) {
        if (!there.contains(objectName(sha.toLatin1())))
            throw Damaged(QStringLiteral("a message is missing (%1)").arg(sha.left(12)).toStdString());
    }
    QStringList pick = c.blobs;
    std::shuffle(pick.begin(), pick.end(), *QRandomGenerator::global());
    for (const QString &sha : pick.mid(0, sample)) {
        if (sha256(getObject(objectName(sha.toLatin1()))) != sha.toLatin1())
            throw Damaged(QStringLiteral("a message does not match its name (%1)").arg(sha.left(12)).toStdString());
    }
}

Contents Repository::restore(const QString &snapshot, const QString &stage, const QByteArray &stageKey,
                             const std::function<void(int)> &progress)
{
    const Contents c = contents(snapshot);
    QDir().mkpath(stage + QStringLiteral("/blobs"));
    const qsizetype total = c.dbChunks.size() + c.blobs.size();
    qsizetype done = 0;
    auto step = [&] {
        if (progress && total)
            progress(int(++done * 100 / total));
    };
    QSaveFile db(stage + QStringLiteral("/mail.db"));
    if (!db.open(QIODevice::WriteOnly))
        throw Damaged("cannot write here");
    QCryptographicHash whole(QCryptographicHash::Sha256);
    for (const QString &n : c.dbChunks) {
        const QByteArray piece = getObject(n);
        whole.addData(piece);
        db.write(piece);
        step();
    }
    if (QString::fromLatin1(whole.result().toHex()) != c.dbSha256)
        throw Damaged("the database does not match its manifest");
    if (!db.commit())
        throw Damaged("cannot write here");
    for (const QString &sha : c.blobs) {
        const QByteArray data = getObject(objectName(sha.toLatin1()));
        if (sha256(data) != sha.toLatin1())
            throw Damaged(QStringLiteral("a message does not match its name (%1)").arg(sha.left(12)).toStdString());
        try {
            BlobStore(stage + QStringLiteral("/blobs"), stageKey).put(data);
        } catch (const std::exception &) {
            throw Damaged("cannot write here");
        }
        step();
    }
    return c;
}

int Repository::prune(const Retention &r, const QDateTime &now)
{
    const QList<Snapshot> all = snapshots(); // newest first
    QHash<QString, QList<Snapshot>> byDevice;
    for (const Snapshot &s : all)
        byDevice[s.device].append(s);
    QStringList keep, drop;
    for (auto it = byDevice.cbegin(); it != byDevice.cend(); ++it) {
        QSet<QString> days, weeks, months;
        bool first = true;
        for (const Snapshot &s : it.value()) {
            const QDate d = s.created.date();
            const QString day = d.toString(Qt::ISODate);
            int isoYear = 0;
            const QString week = QStringLiteral("%1-%2").arg(d.weekNumber(&isoYear)).arg(isoYear);
            const QString month = d.toString(QStringLiteral("yyyy-MM"));
            bool kept = first;
            if (!days.contains(day) && days.size() < r.daily) {
                days.insert(day);
                kept = true;
            }
            if (!weeks.contains(week) && weeks.size() < r.weekly) {
                weeks.insert(week);
                kept = true;
            }
            if (!months.contains(month) && months.size() < r.monthly) {
                months.insert(month);
                kept = true;
            }
            (kept ? keep : drop).append(s.name);
            first = false;
        }
    }
    for (const QString &n : std::as_const(drop))
        m_dest.remove(n);

    // Mark what the kept snapshots need, then sweep the rest, but not what a
    // backup still running may have just put there.
    QSet<QString> needed;
    for (const QString &n : std::as_const(keep)) {
        const Contents c = contents(n);
        for (const QString &x : c.dbChunks)
            needed.insert(x);
        for (const QString &sha : c.blobs)
            needed.insert(objectName(sha.toLatin1()));
    }
    int removed = 0;
    for (const Entry &e : m_dest.list(QStringLiteral("objects"))) {
        const QString name = QStringLiteral("objects/") + e.name;
        if (needed.contains(name))
            continue;
        if (!e.modified.isValid() || e.modified.secsTo(now) < r.graceSeconds)
            continue;
        m_dest.remove(name);
        ++removed;
    }
    return removed;
}

} // namespace e3::backup
