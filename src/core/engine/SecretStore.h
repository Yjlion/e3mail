// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>

#include <optional>

namespace e3 {

// Mail passwords live in the operating system's keyring (Keychain, Credential
// Manager, Secret Service) when one is available. Where none is, the caller
// falls back to the account database and says so in Settings, rather than
// failing to work on a headless machine.
class SecretStore
{
public:
    static bool available();
    static bool write(const QString &key, const QString &value);
    static std::optional<QString> read(const QString &key);
    static void remove(const QString &key);
};

} // namespace e3
