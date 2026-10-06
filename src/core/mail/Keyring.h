// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Types.h"

namespace e3::mail {

// Keys persist in the account database and are loaded into a thread's RNP
// keyring on demand, so a key one thread learned is usable by another.
class Keyring
{
public:
    static void loadAll(MailContext &ctx);
    // Loads `fpr` from the database if this keyring lacks it. Returns whether
    // it is now present.
    static bool ensure(MailContext &ctx, const QString &fpr);
    // Imports key data into the keyring and stores it. Returns primary fprs.
    static QStringList store(MailContext &ctx, const QByteArray &keydata, bool secret = false);
    static QString selfFingerprint(MailContext &ctx);
    // Creates this account's own key if it has none.
    static QString ensureSelfKey(MailContext &ctx, const QString &addr);
};

} // namespace e3::mail
