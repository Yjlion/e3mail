// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Crypto.h"
#include "Destination.h"

#include <QDateTime>
#include <QJsonObject>
#include <QStringList>

#include <stdexcept>

namespace e3 {
class BlobStore;
class Database;
}

// An encrypted backup repository (ADR 0020), one per account, shared by its
// devices. In it:
//   key                 the master key, sealed with a key from the passphrase
//   objects/<hex>       each blob and each piece of the database, sealed;
//                       the name is an HMAC of the content's SHA-256, so
//                       the destination cannot test for known content
//   snapshots/<d>-<t>   a sealed manifest: what one backup holds
// The destination sees only ciphertext, sizes and times.
namespace e3::backup {

class BadPassphrase : public std::runtime_error
{
public:
    BadPassphrase() : std::runtime_error("wrong passphrase") {}
};
// Something in the repository is missing or does not decrypt or match.
class Damaged : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct Snapshot
{
    QString name;    // "snapshots/<device tag>-<UTC time to the millisecond>"
    QString device;  // the device tag
    QDateTime created;
};

struct Contents
{
    QString account;
    QString device;
    QString deviceName;
    QDateTime created;
    int schema = 0;
    QStringList dbChunks; // object names, in order
    QString dbSha256;
    qint64 dbSize = 0;
    QStringList blobs;    // their SHA-256, as BlobStore names them
    QJsonObject secrets;  // passwords and the OAuth refresh token, for a restore
};

class Repository
{
public:
    explicit Repository(Destination &dest) : m_dest(dest) {}

    bool exists();
    // A new repository with a new master key, which it returns.
    QByteArray create(const QByteArray &passphrase, const crypto::KdfParams &params = {});
    // The master key; throws BadPassphrase.
    QByteArray unlock(const QByteArray &passphrase);
    // The master key kept on this device, so scheduled backups need no
    // passphrase.
    void setKey(const QByteArray &master);

    struct Result
    {
        QString snapshot;
        int uploaded = 0;
        int reused = 0;   // already there from an earlier backup
        qint64 bytes = 0; // sent
    };
    // A snapshot of `db` (copied first, without this device's own settings)
    // and every blob in `blobDir`. Only objects not there yet go up.
    // `db` is opened with `dbKey`; the database travels under a key of its
    // own, kept in the sealed manifest's secrets (ADR 0023).
    Result backup(Database &db, const QByteArray &dbKey, const BlobStore &blobs, const Contents &about);
    // Reads back the manifest, every piece of the database and `sample`
    // random blobs, and checks each; throws Damaged.
    void verify(const QString &snapshot, int sample = 16);
    QList<Snapshot> snapshots();
    Contents contents(const QString &snapshot);
    // Writes <stage>/mail.db and <stage>/blobs/, each checked; throws Damaged.
    // Writes the database as it was backed up (its key is in the returned
    // secrets, "dbkey"; none for backups from before ADR 0023) and the blobs
    // sealed with `stageKey`.
    Contents restore(const QString &snapshot, const QString &stage, const QByteArray &stageKey,
                     const std::function<void(int percent)> &progress = {});

    struct Retention
    {
        int daily = 7;
        int weekly = 4;
        int monthly = 6;
        qint64 graceSeconds = 24 * 3600; // objects a backup still running may need
    };
    // Drops snapshots the retention does not keep (per device; the newest
    // always stays), then objects no snapshot needs. Returns objects removed.
    int prune(const Retention &r, const QDateTime &now = QDateTime::currentDateTimeUtc());

    QString deviceTag(const QString &device) const;

private:
    QString objectName(const QByteArray &sha256hex) const;
    void putObject(const QString &name, const QByteArray &plain);
    QByteArray getObject(const QString &name);
    QString nameKeyed(const QByteArray &label, const QByteArray &data) const;

    Destination &m_dest;
    QByteArray m_master, m_kName, m_kObject, m_kSnapshot;
};

} // namespace e3::backup
