// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <stdexcept>

struct rnp_ffi_st;

namespace e3::crypto {

class PgpError : public std::runtime_error
{
public:
    explicit PgpError(const QString &msg) : std::runtime_error(msg.toStdString()) {}
};

// OpenPGP through RNP, in an in-memory keyring owned by this object. e3mail
// never touches the user's GnuPG keyring: an account's keys live in its own
// database and are loaded here. One instance per thread; RNP handles are not
// shared across threads.
class Pgp
{
public:
    Pgp();
    ~Pgp();
    Pgp(const Pgp &) = delete;
    Pgp &operator=(const Pgp &) = delete;

    // Ed25519 primary for signing, Curve25519 subkey for encryption, no
    // expiry, unprotected (the database holding it is what gets protected).
    // Returns the primary key's fingerprint.
    QString generate(const QString &userId);

    // Imports public and/or secret keys. Returns the primary fingerprints
    // that were present in the data.
    QStringList import(const QByteArray &data);

    QByteArray exportPublic(const QString &fpr) const;
    QByteArray exportSecret(const QString &fpr) const;
    // The minimal key Autocrypt wants: primary, one user id, encryption subkey.
    QByteArray exportAutocrypt(const QString &fpr, const QString &addr) const;

    bool hasKey(const QString &fpr) const;
    bool hasSecret(const QString &fpr) const;
    // Present, valid, and has a usable encryption (sub)key.
    bool canEncryptTo(const QString &fpr) const;

    // Encrypts to every recipient and signs with `signer`. ASCII-armored.
    QByteArray encrypt(const QByteArray &plain, const QStringList &recipients, const QString &signer) const;

    struct Decrypted
    {
        QByteArray data;
        bool encrypted = false;      // was protected by encryption
        QStringList validSigners;    // primary fingerprints of good signatures
        QStringList invalidSigners;  // bad, expired, or unknown-key signatures
    };
    // Decrypts and verifies. Throws when the data cannot be decrypted.
    Decrypted decrypt(const QByteArray &data) const;

    static QString normalizeFingerprint(const QString &fpr);

private:
    rnp_ffi_st *m_ffi = nullptr;
};

} // namespace e3::crypto
