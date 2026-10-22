// SPDX-License-Identifier: MPL-2.0
#include "Snapshot.h"
#include "Config.h"
#include "Database.h"

namespace e3 {

void snapshotDatabase(Database &db, const QString &path, const QByteArray &dbKey, const QByteArray &snapshotKey)
{
    QString quoted = path;
    // The copy is encrypted as the database is; it then gets a key of its own.
    db.exec(QStringLiteral("VACUUM INTO '%1'").arg(quoted.replace(u'\'', QStringLiteral("''"))));
    Database snap(path, dbKey);
    for (const char *k : cfg::DeviceOnly)
        snap.run("DELETE FROM config WHERE key=?", k);
    snap.rekey(snapshotKey);
}

} // namespace e3
