// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QString>

#include <stdexcept>

namespace e3 {

class AtRestError : public std::runtime_error
{
public:
    enum class Reason { Passphrase, Keyring, Damaged };
    AtRestError(Reason r, const std::string &what) : std::runtime_error(what), m_reason(r) {}
    Reason reason() const { return m_reason; }

private:
    Reason m_reason;
};

// The key that encrypts an account's database and blobs (ADR 0023): 32
// random bytes per account, made on first use, kept in `<dir>/key.json`
// guarded one of three ways:
//
// - by the OS keyring (the key is there, under account-<id>-dbkey);
// - by an app passphrase (Argon2id, then XChaCha20-Poly1305), when one is
//   set: e3mail then asks for it before opening anything;
// - by nothing (the key is in key.json), where there is neither. Settings
//   says so, as for passwords (ADR 0012).
//
// A database and blobs an older version left in the clear are encrypted
// when the key is first made.
class AtRest
{
public:
    enum class Guard { Keyring, Passphrase, None };

    // This process's passphrase; empty when none is set. Every account opened
    // or re-guarded afterwards is guarded by it.
    static void setPassphrase(const QString &passphrase);
    static bool hasPassphrase();
    // Does any account under `root` need a passphrase to open?
    static bool passphraseRequired(const QString &root);
    // Does `passphrase` open the accounts under `root`?
    static bool checkPassphrase(const QString &root, const QString &passphrase);

    // The account's database key, loaded or made. Throws AtRestError when
    // the passphrase or the keyring cannot give it.
    static QByteArray open(const QString &dir, int accountId);
    // A key for a directory being staged (a restore, a device joining) that
    // has no account id yet: guarded by nothing until `open` adopts it.
    static QByteArray stage(const QString &dir);
    // Guards the key again: by the passphrase if one is set, else the
    // keyring if there is one, else nothing.
    static void reguard(const QString &dir, int accountId, const QByteArray &key);
    static Guard guard(const QString &dir);
    // Removes what the keyring holds for an account being removed.
    static void forget(int accountId);
};

} // namespace e3
