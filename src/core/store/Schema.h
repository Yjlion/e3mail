// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace e3 {

class Database;

// Brings an account database up to the current schema. Migrations are
// append-only and each runs in its own transaction; `PRAGMA user_version`
// records how far a database has got.
class Schema
{
public:
    static int currentVersion();
    static void migrate(Database &db);
};

} // namespace e3
