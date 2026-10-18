// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>

namespace e3 {

class Database;

// A consistent copy of an account's database at `path` (VACUUM INTO), without
// the settings that belong to this device alone (cfg::DeviceOnly). Pairing
// sends it; backups seal it.
void snapshotDatabase(Database &db, const QString &path);

} // namespace e3
