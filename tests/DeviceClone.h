// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>

namespace e3 {
class Account;
}

// A second device of an account, made the way pairing's snapshot will make
// it: the account directory copied under `root`, with a new device id and
// without this device's endpoint key. Returns false if a copy failed. Open
// it with an AccountManager on `root`.
bool cloneDevice(e3::Account *src, const QString &root);
