// SPDX-License-Identifier: MPL-2.0
#include "Backups.h"

#include "Account.h"
#include "AccountManager.h"
#include "backup/Remote.h"
#include "net/SyncSocket.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"
#include "store/Schema.h"
#include "util/Log.h"

#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtConcurrentRun>

namespace e3::backups {

namespace {
const char *const kDestSecret = "backup-dest";
const char *const kKey = "backup";
} // namespace

std::unique_ptr<backup::Destination> destination(const QJsonObject &c, const QString &secret)
{
    const QString type = c.value(QLatin1String("type")).toString();
    if (type == QLatin1String("folder"))
        return std::make_unique<backup::FolderDestination>(c.value(QLatin1String("path")).toString());
    return backup::remoteDestination(c, secret);
}

bool isSetUp(Account &a)
{
    return a.ctx().config.isSet(cfg::BackupDest) && !a.secret(kKey, cfg::BackupKey).isEmpty();
}

QJsonObject config(Account &a)
{
    return QJsonDocument::fromJson(a.ctx().config.get(cfg::BackupDest).toUtf8()).object();
}

QByteArray openOrCreate(const QJsonObject &config, const QString &secret, const QByteArray &passphrase,
                        const backup::crypto::KdfParams &params)
{
    auto dest = destination(config, secret);
    if (!dest)
        throw net::NetError(QStringLiteral("unknown backup destination"));
    backup::Repository repo(*dest);
    // Another device of this account may have made it already: then the
    // passphrase opens it, and both back up into it.
    return repo.exists() ? repo.unlock(passphrase) : repo.create(passphrase, params);
}

void store(Account &a, const QJsonObject &config, const QString &secret, const QByteArray &master)
{
    a.ctx().config.set(cfg::BackupDest, QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Compact)));
    a.storeSecret(kDestSecret, cfg::BackupDestSecret, secret);
    a.storeSecret(kKey, cfg::BackupKey, QString::fromLatin1(master.toBase64()));
    a.ctx().config.set(cfg::BackupLastError, QString());
}

void setUp(Account &a, const QJsonObject &config, const QString &secret, const QByteArray &passphrase,
           const backup::crypto::KdfParams &params)
{
    store(a, config, secret, openOrCreate(config, secret, passphrase, params));
}

void forget(Account &a)
{
    a.ctx().db.run("DELETE FROM config WHERE key IN (?, ?, ?)", cfg::BackupDest, cfg::BackupLast, cfg::BackupLastError);
    a.storeSecret(kDestSecret, cfg::BackupDestSecret, QString());
    a.storeSecret(kKey, cfg::BackupKey, QString());
}

QFuture<Outcome> run(Account &a)
{
    const QString dir = a.dir();
    const QJsonObject c = config(a);
    const QString secret = a.secret(kDestSecret, cfg::BackupDestSecret);
    const QByteArray master = QByteArray::fromBase64(a.secret(kKey, cfg::BackupKey).toLatin1());
    const Account::Settings s = a.settings();
    backup::Contents about;
    about.account = s.addr;
    about.device = a.ctx().ops.deviceId();
    about.deviceName = a.ctx().config.get(cfg::DeviceName);
    // What a restore needs to connect again: inside the sealed manifest only.
    about.secrets = QJsonObject{{QStringLiteral("in"), s.inPassword},
                                {QStringLiteral("smtp"), s.smtpPassword},
                                {QStringLiteral("oauth"), s.oauthRefresh}};
    return QtConcurrent::run([dir, c, secret, master, about]() -> Outcome {
        Outcome o;
        try {
            if (master.size() != 32)
                throw backup::Damaged("backups are not set up on this device");
            auto dest = destination(c, secret);
            if (!dest)
                throw net::NetError(QStringLiteral("unknown backup destination"));
            backup::Repository repo(*dest);
            repo.setKey(master);
            // Its own connection: this runs beside the app's.
            Database db(dir + QStringLiteral("/mail.db"));
            o.result = repo.backup(db, dir + QStringLiteral("/blobs"), about);
            // A backup that cannot be read back is not a backup.
            repo.verify(o.result.snapshot);
            repo.prune({});
            o.ok = true;
        } catch (const std::exception &e) {
            o.error = QString::fromUtf8(e.what());
        }
        return o;
    });
}

void record(Account &a, const Outcome &o)
{
    if (o.ok) {
        a.ctx().config.set(cfg::BackupLast, QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        a.ctx().config.set(cfg::BackupLastError, QString());
    } else {
        a.ctx().config.set(cfg::BackupLastError, o.error);
        qCWarning(lcEngine) << "backup failed:" << o.error;
    }
}

Staged stage(const QString &root, backup::Destination &dest, const QByteArray &passphrase, const QString &snapshot,
             const QString &deviceName, const std::function<void(int)> &progress)
{
    backup::Repository repo(dest);
    Staged s;
    s.master = repo.unlock(passphrase);
    QString which = snapshot;
    if (which.isEmpty()) {
        const auto all = repo.snapshots();
        if (all.isEmpty())
            throw backup::Damaged("there are no backups there");
        which = all.first().name;
    }
    // Staged beside the accounts, so adopting it is a rename.
    QDir().mkpath(root);
    QTemporaryDir tmp(root + QStringLiteral("/.restore-XXXXXX"));
    try {
        s.contents = repo.restore(which, tmp.path(), progress);
        Database db(tmp.path() + QStringLiteral("/mail.db"));
        Schema::migrate(db);
        // A device of its own: a new id (made on first use) and key, and no
        // peers. The others do not know it; it can be paired again.
        db.run("DELETE FROM devices");
        Config c(db);
        c.set(cfg::DeviceName, deviceName);
    } catch (...) {
        throw; // tmp removes what was written
    }
    tmp.setAutoRemove(false);
    s.dir = tmp.path();
    return s;
}

Account *adopt(AccountManager &mgr, const Staged &staged)
{
    Account *a = mgr.adopt(staged.dir);
    if (!a) {
        QDir(staged.dir).removeRecursively();
        throw backup::Damaged("cannot write here");
    }
    const QJsonObject s = staged.contents.secrets;
    a->storePasswords(s.value(QLatin1String("in")).toString(), s.value(QLatin1String("smtp")).toString());
    a->storeRefreshToken(s.value(QLatin1String("oauth")).toString());
    a->storeSecret(kKey, cfg::BackupKey, QString::fromLatin1(staged.master.toBase64()));
    return a;
}

Account *restore(AccountManager &mgr, backup::Destination &dest, const QByteArray &passphrase,
                 const QString &snapshot, const QString &deviceName, const std::function<void(int)> &progress)
{
    return adopt(mgr, stage(mgr.root(), dest, passphrase, snapshot, deviceName, progress));
}

} // namespace e3::backups
