// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "backup/Repository.h"

#include <QFuture>
#include <QJsonObject>
#include <QString>

#include <memory>

namespace e3 {

class Account;
class AccountManager;

// An account's encrypted backups (ADR 0020): where they go, the key, running
// one, and restoring into a new account. Per device: each device that backs
// up an account is set up on its own, into the same repository.
namespace backups {

// What a destination needs, without its secret:
//   {type: "folder", path}
//   {type: "webdav", url, user, pin?}
//   {type: "s3", endpoint, region, bucket, prefix, accessKey, pathStyle}
//   {type: "sftp", host, port, user, path, hostKey?}
//   {type: "gdrive" | "onedrive", folder}
// The secret is a password, an S3 secret key, or an OAuth refresh token.
std::unique_ptr<backup::Destination> destination(const QJsonObject &config, const QString &secret);

bool isSetUp(Account &a);
QJsonObject config(Account &a);
// Sets the destination up and makes (or opens, with the passphrase, if one
// is there) the repository. Throws backup::BadPassphrase, net::NetError.
void setUp(Account &a, const QJsonObject &config, const QString &secret, const QByteArray &passphrase,
           const backup::crypto::KdfParams &params = {});
// The same in two steps, for the app: the slow part on any thread (the
// network, Argon2id), returning the master key; then storing, on the
// account's.
QByteArray openOrCreate(const QJsonObject &config, const QString &secret, const QByteArray &passphrase,
                        const backup::crypto::KdfParams &params = {});
void store(Account &a, const QJsonObject &config, const QString &secret, const QByteArray &master);
void forget(Account &a);

struct Outcome
{
    bool ok = false;
    QString error;
    backup::Repository::Result result;
};
// Backs up, checks what it wrote, prunes; on a thread of its own, with a
// database connection of its own. Records the outcome in the account.
QFuture<Outcome> run(Account &a);
// Records an outcome (on the account's thread).
void record(Account &a, const Outcome &o);

// A new account from a snapshot (empty: the newest). `deviceName`: this
// device's. Throws backup::Damaged, backup::BadPassphrase, net::NetError.
Account *restore(AccountManager &mgr, backup::Destination &dest, const QByteArray &passphrase,
                 const QString &snapshot, const QString &deviceName,
                 const std::function<void(int)> &progress = {});
// The same in two steps: fetched and checked into a folder under `root` (any
// thread), then taken in as an account (the manager's thread).
struct Staged
{
    QString dir;
    backup::Contents contents;
    QByteArray master;
};
Staged stage(const QString &root, backup::Destination &dest, const QByteArray &passphrase, const QString &snapshot,
             const QString &deviceName, const std::function<void(int)> &progress = {});
Account *adopt(AccountManager &mgr, const Staged &staged);

} // namespace backups
} // namespace e3
