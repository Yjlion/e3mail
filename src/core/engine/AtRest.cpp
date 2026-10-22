// SPDX-License-Identifier: MPL-2.0
#include "AtRest.h"

#include "SecretStore.h"
#include "backup/Crypto.h"
#include "store/BlobStore.h"
#include "store/Database.h"
#include "util/Log.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSaveFile>

namespace e3 {

namespace {

namespace bc = backup::crypto;

constexpr char kFile[] = "/key.json";
constexpr char kWrapAd[] = "e3mail database key v1";

QMutex g_lock;
QString g_passphrase;
QHash<QByteArray, QByteArray> g_kek; // salt -> key from the passphrase

QString keyringName(int accountId)
{
    return QStringLiteral("account-%1-dbkey").arg(accountId);
}

QJsonObject readFile(const QString &dir)
{
    QFile f(dir + QLatin1String(kFile));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

void writeFile(const QString &dir, const QJsonObject &o)
{
    QSaveFile f(dir + QLatin1String(kFile));
    const QByteArray data = QJsonDocument(o).toJson();
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
        throw AtRestError(AtRestError::Reason::Damaged, "cannot write " + f.fileName().toStdString());
    QFile::setPermissions(f.fileName(), QFile::ReadOwner | QFile::WriteOwner);
}

bc::KdfParams paramsFrom(const QJsonObject &o)
{
    bc::KdfParams p;
    p.salt = QByteArray::fromBase64(o.value(QLatin1String("salt")).toString().toLatin1());
    p.memoryKiB = quint32(o.value(QLatin1String("m")).toInt(int(p.memoryKiB)));
    p.iterations = quint32(o.value(QLatin1String("t")).toInt(int(p.iterations)));
    p.parallelism = quint32(o.value(QLatin1String("p")).toInt(int(p.parallelism)));
    return p;
}

// Argon2id is slow on purpose; each salt is stretched once per process.
QByteArray kekFor(const QString &passphrase, const bc::KdfParams &p)
{
    QMutexLocker lock(&g_lock);
    const QByteArray cacheKey = p.salt + passphrase.toUtf8();
    if (const auto it = g_kek.constFind(cacheKey); it != g_kek.cend())
        return *it;
    const QByteArray kek = bc::argon2id(passphrase.toUtf8(), p);
    g_kek.insert(cacheKey, kek);
    return kek;
}

QString passphrase()
{
    QMutexLocker lock(&g_lock);
    return g_passphrase;
}

std::optional<QByteArray> unwrap(const QJsonObject &o, const QString &pass)
{
    if (pass.isEmpty())
        return std::nullopt;
    const QByteArray wrapped = QByteArray::fromBase64(o.value(QLatin1String("wrapped")).toString().toLatin1());
    return bc::open(kekFor(pass, paramsFrom(o)), wrapped, kWrapAd);
}

AtRest::Guard guardOf(const QJsonObject &o)
{
    const QString g = o.value(QLatin1String("guard")).toString();
    return g == QLatin1String("keyring") ? AtRest::Guard::Keyring
        : g == QLatin1String("passphrase") ? AtRest::Guard::Passphrase
                                             : AtRest::Guard::None;
}

AtRest::Guard wanted()
{
    if (!passphrase().isEmpty())
        return AtRest::Guard::Passphrase;
    return SecretStore::available() ? AtRest::Guard::Keyring : AtRest::Guard::None;
}

// Writes key.json for `key` under `g`; falls back to no guard when the
// keyring will not take it. Returns the guard used.
AtRest::Guard store(const QString &dir, int accountId, const QByteArray &key, AtRest::Guard g)
{
    QJsonObject o{{QStringLiteral("v"), 1}};
    const QJsonObject before = readFile(dir);
    if (before.value(QLatin1String("blobs")).toBool())
        o.insert(QStringLiteral("blobs"), true);
    if (g == AtRest::Guard::Passphrase) {
        bc::KdfParams p;
        p.salt = bc::random(16);
        o.insert(QStringLiteral("guard"), QStringLiteral("passphrase"));
        o.insert(QStringLiteral("salt"), QString::fromLatin1(p.salt.toBase64()));
        o.insert(QStringLiteral("m"), int(p.memoryKiB));
        o.insert(QStringLiteral("t"), int(p.iterations));
        o.insert(QStringLiteral("p"), int(p.parallelism));
        o.insert(QStringLiteral("wrapped"),
                 QString::fromLatin1(bc::seal(kekFor(passphrase(), p), key, kWrapAd).toBase64()));
    } else if (g == AtRest::Guard::Keyring && accountId > 0
               && SecretStore::write(keyringName(accountId), QString::fromLatin1(key.toHex()))) {
        o.insert(QStringLiteral("guard"), QStringLiteral("keyring"));
    } else {
        g = AtRest::Guard::None;
        o.insert(QStringLiteral("guard"), QStringLiteral("none"));
        o.insert(QStringLiteral("key"), QString::fromLatin1(key.toHex()));
    }
    writeFile(dir, o);
    // Written first, removed after: a crash between leaves the key in two
    // places, never in none.
    if (g != AtRest::Guard::Keyring && accountId > 0 && guardOf(before) == AtRest::Guard::Keyring)
        SecretStore::remove(keyringName(accountId));
    return g;
}

// Encrypts what an older version left in the clear: the database, then
// the blobs, which may take a while and resume on the next start.
void sealLegacy(const QString &dir, const QByteArray &key)
{
    const QString db = dir + QStringLiteral("/mail.db");
    if (QFile::exists(db) && Database::isPlaintext(db)) {
        Database plain(db);
        plain.rekey(key);
        qCInfo(lcEngine) << "encrypted" << db;
    }
    QJsonObject o = readFile(dir);
    if (!o.value(QLatin1String("blobs")).toBool()) {
        BlobStore(dir + QStringLiteral("/blobs"), key).encryptLegacy();
        o.insert(QStringLiteral("blobs"), true);
        writeFile(dir, o);
    }
}

} // namespace

void AtRest::setPassphrase(const QString &p)
{
    QMutexLocker lock(&g_lock);
    g_passphrase = p;
}

bool AtRest::hasPassphrase()
{
    return !passphrase().isEmpty();
}

bool AtRest::passphraseRequired(const QString &root)
{
    for (const QString &d : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (guardOf(readFile(root + u'/' + d)) == Guard::Passphrase && QFile::exists(root + u'/' + d + QLatin1String(kFile)))
            return true;
    }
    return false;
}

bool AtRest::checkPassphrase(const QString &root, const QString &pass)
{
    for (const QString &d : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QJsonObject o = readFile(root + u'/' + d);
        if (guardOf(o) == Guard::Passphrase && o.contains(QLatin1String("wrapped")))
            return unwrap(o, pass).has_value();
    }
    return true; // nothing guarded by one
}

QByteArray AtRest::open(const QString &dir, int accountId)
{
    QDir().mkpath(dir);
    const QJsonObject o = readFile(dir);
    QByteArray key;
    Guard g = guardOf(o);
    if (o.isEmpty()) {
        key = bc::random(32);
        g = store(dir, accountId, key, wanted());
    } else if (g == Guard::Keyring) {
        const auto hex = SecretStore::read(keyringName(accountId));
        if (!hex)
            throw AtRestError(AtRestError::Reason::Keyring, "the keyring did not give this account's key");
        key = QByteArray::fromHex(hex->toLatin1());
    } else if (g == Guard::Passphrase) {
        const auto k = unwrap(o, passphrase());
        if (!k)
            throw AtRestError(AtRestError::Reason::Passphrase, "the passphrase does not open this account");
        key = *k;
    } else {
        key = QByteArray::fromHex(o.value(QLatin1String("key")).toString().toLatin1());
    }
    if (key.size() != 32)
        throw AtRestError(AtRestError::Reason::Damaged, "this account's key is damaged");
    // A staged copy, a machine that gained a keyring, a passphrase set or
    // removed: guarded as asked now.
    if (const Guard w = wanted(); w != g)
        store(dir, accountId, key, w);
    sealLegacy(dir, key);
    return key;
}

QByteArray AtRest::stage(const QString &dir)
{
    QDir().mkpath(dir);
    const QByteArray key = bc::random(32);
    store(dir, 0, key, Guard::None);
    return key;
}

void AtRest::reguard(const QString &dir, int accountId, const QByteArray &key)
{
    store(dir, accountId, key, wanted());
}

AtRest::Guard AtRest::guard(const QString &dir)
{
    return guardOf(readFile(dir));
}

void AtRest::forget(int accountId)
{
    SecretStore::remove(keyringName(accountId));
}

} // namespace e3
