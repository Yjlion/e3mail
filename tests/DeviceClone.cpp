// SPDX-License-Identifier: MPL-2.0
#include "DeviceClone.h"

#include "engine/Account.h"
#include "store/Database.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

using namespace e3;

bool cloneDevice(Account *src, const QString &root)
{
    src->ctx().db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
    const QString dst = root + QStringLiteral("/accounts/") + QString::number(src->id());
    QDirIterator it(src->dir(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString from = it.next();
        const QString to = dst + from.mid(src->dir().size());
        QDir().mkpath(QFileInfo(to).path());
        if (!QFile::copy(from, to))
            return false;
    }
    {
        Database db(dst + QStringLiteral("/mail.db"));
        db.run("UPDATE config SET value=? WHERE key='device_id'", QUuid::createUuid().toString(QUuid::WithoutBraces));
        db.run("DELETE FROM config WHERE key='p2p_secret'");
        // Backups are set up per device (ADR 0020), as pairing's copy leaves them.
        db.run("DELETE FROM config WHERE key LIKE 'backup_%'");
    }
    QFile f(root + QStringLiteral("/accounts/accounts.json"));
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(QJsonObject{{QStringLiteral("accounts"), QJsonArray{src->id()}},
                                      {QStringLiteral("next_id"), src->id() + 1}})
                .toJson());
    return true;
}
