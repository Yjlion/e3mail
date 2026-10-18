// SPDX-License-Identifier: MPL-2.0
#include "Snapshot.h"
#include "Config.h"
#include "Database.h"

namespace e3 {

void snapshotDatabase(Database &db, const QString &path)
{
    QString quoted = path;
    db.exec(QStringLiteral("VACUUM INTO '%1'").arg(quoted.replace(u'\'', QStringLiteral("''"))));
    Database snap(path);
    for (const char *k : cfg::DeviceOnly)
        snap.run("DELETE FROM config WHERE key=?", k);
    snap.exec("PRAGMA wal_checkpoint(TRUNCATE)");
}

} // namespace e3
