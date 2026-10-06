// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Types.h"

namespace e3::mail {

// The settings a person changes. Those that are the account's behaviour
// travel to the other devices as ops; raw-message retention and polling stay
// with each device, and server settings and secrets never travel (ADR 0013).
class Preferences
{
public:
    static bool isSynced(const QString &key);

    // Stores the value, applies its side effects, and records it if synced.
    static void set(MailContext &ctx, const char *key, const QString &value);

    // Stores the value and applies its side effects, without recording. For
    // ops from other devices.
    static void apply(MailContext &ctx, const QString &key, const QString &value);
};

} // namespace e3::mail
