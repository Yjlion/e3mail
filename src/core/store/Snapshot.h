// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QString>

namespace e3 {

class Database;

// A consistent copy of an account's database at `path` (VACUUM INTO), without
// the settings that belong to this device alone (cfg::DeviceOnly), encrypted
// with `snapshotKey` (ADR 0023): the copy never carries the account's own
// key. Pairing sends it; backups seal it.
void snapshotDatabase(Database &db, const QString &path, const QByteArray &dbKey, const QByteArray &snapshotKey);

} // namespace e3
