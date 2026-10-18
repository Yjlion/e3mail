// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>

#include <optional>

// The encryption of backups (ADR 0020), through Botan: Argon2id from a
// passphrase, ChaCha20-Poly1305 with a 192-bit random nonce (XChaCha20) for
// every object, HMAC-SHA-256 for names. Everything is sealed on the device
// before it leaves it.
namespace e3::backup::crypto {

struct KdfParams
{
    quint32 memoryKiB = 64 * 1024;
    quint32 iterations = 3;
    quint32 parallelism = 1;
    QByteArray salt; // 16 random bytes
};

QByteArray random(int bytes);
// A 32-byte key from a passphrase.
QByteArray argon2id(const QByteArray &passphrase, const KdfParams &params);
// nonce (24) || ciphertext || tag (16), bound to `ad` (the object's name).
QByteArray seal(const QByteArray &key, const QByteArray &plain, const QByteArray &ad);
// Nothing if the key, the name or a single byte is wrong.
std::optional<QByteArray> open(const QByteArray &key, const QByteArray &sealed, const QByteArray &ad);
QByteArray hmac(const QByteArray &key, const QByteArray &data);

} // namespace e3::backup::crypto
