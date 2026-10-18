// SPDX-License-Identifier: MPL-2.0
#include "Crypto.h"

#include <botan/aead.h>
#include <botan/auto_rng.h>
#include <botan/build.h>
#include <botan/mac.h>
#include <botan/pwdhash.h>

#include <stdexcept>

namespace e3::backup::crypto {

namespace {

constexpr int kNonce = 24;
constexpr int kTag = 16;

const uint8_t *bytes(const QByteArray &b)
{
    return reinterpret_cast<const uint8_t *>(b.constData());
}

std::unique_ptr<Botan::AEAD_Mode> aead(bool encrypt)
{
#if BOTAN_VERSION_MAJOR >= 3
    const auto dir = encrypt ? Botan::Cipher_Dir::Encryption : Botan::Cipher_Dir::Decryption;
#else
    const auto dir = encrypt ? Botan::ENCRYPTION : Botan::DECRYPTION;
#endif
    auto mode = Botan::AEAD_Mode::create("ChaCha20Poly1305", dir);
    if (!mode)
        throw std::runtime_error("this Botan has no ChaCha20Poly1305");
    return mode;
}

} // namespace

QByteArray random(int n)
{
    static thread_local Botan::AutoSeeded_RNG rng;
    QByteArray out(n, Qt::Uninitialized);
    rng.randomize(reinterpret_cast<uint8_t *>(out.data()), size_t(n));
    return out;
}

QByteArray argon2id(const QByteArray &passphrase, const KdfParams &p)
{
    auto family = Botan::PasswordHashFamily::create("Argon2id");
    if (!family)
        throw std::runtime_error("this Botan has no Argon2id");
    auto kdf = family->from_params(p.memoryKiB, p.iterations, p.parallelism);
    QByteArray key(32, Qt::Uninitialized);
    kdf->derive_key(reinterpret_cast<uint8_t *>(key.data()), size_t(key.size()), passphrase.constData(),
                    size_t(passphrase.size()), bytes(p.salt), size_t(p.salt.size()));
    return key;
}

QByteArray seal(const QByteArray &key, const QByteArray &plain, const QByteArray &ad)
{
    auto mode = aead(true);
    mode->set_key(bytes(key), size_t(key.size()));
    mode->set_associated_data(bytes(ad), size_t(ad.size()));
    const QByteArray nonce = random(kNonce);
    mode->start(bytes(nonce), size_t(nonce.size()));
    Botan::secure_vector<uint8_t> buf(plain.cbegin(), plain.cend());
    mode->finish(buf);
    return nonce + QByteArray(reinterpret_cast<const char *>(buf.data()), qsizetype(buf.size()));
}

std::optional<QByteArray> open(const QByteArray &key, const QByteArray &sealed, const QByteArray &ad)
{
    if (sealed.size() < kNonce + kTag)
        return std::nullopt;
    try {
        auto mode = aead(false);
        mode->set_key(bytes(key), size_t(key.size()));
        mode->set_associated_data(bytes(ad), size_t(ad.size()));
        mode->start(bytes(sealed), kNonce);
        Botan::secure_vector<uint8_t> buf(sealed.cbegin() + kNonce, sealed.cend());
        mode->finish(buf);
        return QByteArray(reinterpret_cast<const char *>(buf.data()), qsizetype(buf.size()));
    } catch (const std::exception &) {
        return std::nullopt; // Botan::Invalid_Authentication_Tag and the like
    }
}

QByteArray hmac(const QByteArray &key, const QByteArray &data)
{
    auto mac = Botan::MessageAuthenticationCode::create("HMAC(SHA-256)");
    mac->set_key(bytes(key), size_t(key.size()));
    mac->update(bytes(data), size_t(data.size()));
    const auto out = mac->final();
    return QByteArray(reinterpret_cast<const char *>(out.data()), qsizetype(out.size()));
}

} // namespace e3::backup::crypto
