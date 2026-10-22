// SPDX-License-Identifier: MPL-2.0
#include "Pgp.h"

#include "util/Log.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <rnp/rnp.h>
#include <rnp/rnp_err.h>

#include <memory>
#include <optional>

namespace e3::crypto {

namespace {

void check(rnp_result_t rc, const char *what)
{
    if (rc != RNP_SUCCESS)
        throw PgpError(QStringLiteral("%1 failed: %2").arg(QLatin1String(what), QLatin1String(rnp_result_to_string(rc))));
}

struct InputDeleter
{
    void operator()(rnp_input_st *p) const { rnp_input_destroy(p); }
};
struct OutputDeleter
{
    void operator()(rnp_output_st *p) const { rnp_output_destroy(p); }
};
struct KeyDeleter
{
    void operator()(rnp_key_handle_st *p) const { rnp_key_handle_destroy(p); }
};
using Input = std::unique_ptr<rnp_input_st, InputDeleter>;
using Output = std::unique_ptr<rnp_output_st, OutputDeleter>;
using Key = std::unique_ptr<rnp_key_handle_st, KeyDeleter>;

Input inputFrom(const QByteArray &data)
{
    rnp_input_t in = nullptr;
    check(rnp_input_from_memory(&in, reinterpret_cast<const uint8_t *>(data.constData()), size_t(data.size()), true),
          "rnp_input_from_memory");
    return Input(in);
}

Output memoryOutput()
{
    rnp_output_t out = nullptr;
    check(rnp_output_to_memory(&out, 0), "rnp_output_to_memory");
    return Output(out);
}

QByteArray bytesOf(rnp_output_t out)
{
    uint8_t *buf = nullptr;
    size_t len = 0;
    check(rnp_output_memory_get_buf(out, &buf, &len, false), "rnp_output_memory_get_buf");
    return QByteArray(reinterpret_cast<const char *>(buf), qsizetype(len));
}

QString takeString(char *s)
{
    const QString out = QString::fromUtf8(s);
    rnp_buffer_destroy(s);
    return out;
}

Key locate(rnp_ffi_t ffi, const char *type, const QString &id)
{
    rnp_key_handle_t key = nullptr;
    if (rnp_locate_key(ffi, type, id.toUtf8().constData(), &key) != RNP_SUCCESS)
        return nullptr;
    return Key(key);
}

QString fingerprintOf(rnp_key_handle_t key)
{
    char *fp = nullptr;
    check(rnp_key_get_fprint(key, &fp), "rnp_key_get_fprint");
    return takeString(fp);
}

QString primaryFingerprintOf(rnp_key_handle_t key)
{
    bool primary = false;
    rnp_key_is_primary(key, &primary);
    if (primary)
        return fingerprintOf(key);
    char *fp = nullptr;
    if (rnp_key_get_primary_fprint(key, &fp) != RNP_SUCCESS)
        return fingerprintOf(key);
    return takeString(fp);
}

} // namespace

namespace {

// Primary fingerprints of the good signatures, and of the rest.
void signersOf(rnp_op_verify_t op, QStringList &valid, QStringList &invalid)
{
    size_t count = 0;
    rnp_op_verify_get_signature_count(op, &count);
    for (size_t i = 0; i < count; ++i) {
        rnp_op_verify_signature_t sig = nullptr;
        if (rnp_op_verify_get_signature_at(op, i, &sig) != RNP_SUCCESS)
            continue;
        const rnp_result_t status = rnp_op_verify_signature_get_status(sig);
        rnp_key_handle_t keyHandle = nullptr;
        QString fpr;
        if (rnp_op_verify_signature_get_key(sig, &keyHandle) == RNP_SUCCESS && keyHandle) {
            const Key key(keyHandle);
            fpr = primaryFingerprintOf(key.get());
        }
        if (status == RNP_SUCCESS && !fpr.isEmpty())
            valid.append(fpr);
        else if (!fpr.isEmpty())
            invalid.append(fpr);
    }
}

} // namespace

QString Pgp::normalizeFingerprint(const QString &fpr)
{
    QString out = fpr.toUpper();
    out.remove(u' ');
    return out;
}

Pgp::Pgp()
{
    check(rnp_ffi_create(&m_ffi, "GPG", "GPG"), "rnp_ffi_create");
}

Pgp::~Pgp()
{
    rnp_ffi_destroy(m_ffi);
}

QString Pgp::generate(const QString &userId)
{
    QJsonObject primary{{QStringLiteral("type"), QStringLiteral("EDDSA")},
                        {QStringLiteral("userid"), userId},
                        {QStringLiteral("usage"), QJsonArray{QStringLiteral("sign"), QStringLiteral("certify")}},
                        {QStringLiteral("expiration"), 0}};
    QJsonObject sub{{QStringLiteral("type"), QStringLiteral("ECDH")},
                    {QStringLiteral("curve"), QStringLiteral("Curve25519")},
                    {QStringLiteral("usage"), QJsonArray{QStringLiteral("encrypt")}},
                    {QStringLiteral("expiration"), 0}};
    const QByteArray json =
        QJsonDocument(QJsonObject{{QStringLiteral("primary"), primary}, {QStringLiteral("sub"), sub}}).toJson();
    char *results = nullptr;
    check(rnp_generate_key_json(m_ffi, json.constData(), &results), "rnp_generate_key_json");
    const QJsonObject res = QJsonDocument::fromJson(takeString(results).toUtf8()).object();
    const QString grip = res.value(QStringLiteral("primary")).toObject().value(QStringLiteral("grip")).toString();
    const Key key = locate(m_ffi, "grip", grip);
    if (!key)
        throw PgpError(QStringLiteral("generated key not found"));
    return fingerprintOf(key.get());
}

QStringList Pgp::import(const QByteArray &data)
{
    const Input in = inputFrom(data);
    char *results = nullptr;
    check(rnp_import_keys(m_ffi, in.get(),
                          RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS | RNP_LOAD_SAVE_PERMISSIVE,
                          &results),
          "rnp_import_keys");
    const QJsonObject res = QJsonDocument::fromJson(takeString(results).toUtf8()).object();
    QStringList primaries;
    for (const QJsonValue &v : res.value(QStringLiteral("keys")).toArray()) {
        const QString fpr = v.toObject().value(QStringLiteral("fingerprint")).toString();
        const Key key = locate(m_ffi, "fingerprint", fpr);
        if (!key)
            continue;
        const QString primary = primaryFingerprintOf(key.get());
        if (!primaries.contains(primary))
            primaries.append(primary);
    }
    return primaries;
}

QByteArray Pgp::exportPublic(const QString &fpr, bool armored) const
{
    const Key key = locate(m_ffi, "fingerprint", fpr);
    if (!key)
        throw PgpError(QStringLiteral("no key %1").arg(fpr));
    const Output out = memoryOutput();
    check(rnp_key_export(key.get(), out.get(),
                         RNP_KEY_EXPORT_PUBLIC | RNP_KEY_EXPORT_SUBKEYS | (armored ? RNP_KEY_EXPORT_ARMORED : 0)),
          "rnp_key_export");
    return bytesOf(out.get());
}

QByteArray Pgp::exportSecret(const QString &fpr) const
{
    const Key key = locate(m_ffi, "fingerprint", fpr);
    if (!key)
        throw PgpError(QStringLiteral("no key %1").arg(fpr));
    const Output out = memoryOutput();
    check(rnp_key_export(key.get(), out.get(), RNP_KEY_EXPORT_SECRET | RNP_KEY_EXPORT_SUBKEYS), "rnp_key_export");
    return bytesOf(out.get());
}

QByteArray Pgp::exportAutocrypt(const QString &fpr, const QString &addr) const
{
    const Key key = locate(m_ffi, "fingerprint", fpr);
    if (!key)
        throw PgpError(QStringLiteral("no key %1").arg(fpr));
    // Pick the user id that names this address, if any; RNP needs an exact match.
    QByteArray uid;
    size_t count = 0;
    rnp_key_get_uid_count(key.get(), &count);
    for (size_t i = 0; i < count; ++i) {
        char *u = nullptr;
        if (rnp_key_get_uid_at(key.get(), i, &u) == RNP_SUCCESS) {
            const QString s = takeString(u);
            if (s.contains(addr, Qt::CaseInsensitive) || uid.isEmpty())
                uid = s.toUtf8();
        }
    }
    const Output out = memoryOutput();
    check(rnp_key_export_autocrypt(key.get(), nullptr, uid.isEmpty() ? nullptr : uid.constData(), out.get(), 0),
          "rnp_key_export_autocrypt");
    return bytesOf(out.get());
}

bool Pgp::hasKey(const QString &fpr) const
{
    return bool(locate(m_ffi, "fingerprint", fpr));
}

bool Pgp::hasSecret(const QString &fpr) const
{
    const Key key = locate(m_ffi, "fingerprint", fpr);
    bool secret = false;
    return key && rnp_key_have_secret(key.get(), &secret) == RNP_SUCCESS && secret;
}

bool Pgp::canEncryptTo(const QString &fpr) const
{
    const Key key = locate(m_ffi, "fingerprint", fpr);
    if (!key)
        return false;
    bool valid = false;
    if (rnp_key_is_valid(key.get(), &valid) != RNP_SUCCESS || !valid)
        return false;
    auto usable = [](rnp_key_handle_t k) {
        bool ok = false, v = false;
        return rnp_key_allows_usage(k, "encrypt", &ok) == RNP_SUCCESS && ok
            && rnp_key_is_valid(k, &v) == RNP_SUCCESS && v;
    };
    if (usable(key.get()))
        return true;
    size_t subs = 0;
    rnp_key_get_subkey_count(key.get(), &subs);
    for (size_t i = 0; i < subs; ++i) {
        rnp_key_handle_t sub = nullptr;
        if (rnp_key_get_subkey_at(key.get(), i, &sub) != RNP_SUCCESS)
            continue;
        const Key guard(sub);
        if (usable(sub))
            return true;
    }
    return false;
}

QByteArray Pgp::encrypt(const QByteArray &plain, const QStringList &recipients, const QString &signer) const
{
    const Input in = inputFrom(plain);
    const Output out = memoryOutput();
    rnp_op_encrypt_t op = nullptr;
    check(rnp_op_encrypt_create(&op, m_ffi, in.get(), out.get()), "rnp_op_encrypt_create");
    const std::unique_ptr<rnp_op_encrypt_st, decltype(&rnp_op_encrypt_destroy)> guard(op, &rnp_op_encrypt_destroy);

    std::vector<Key> keys;
    for (const QString &fpr : recipients) {
        Key key = locate(m_ffi, "fingerprint", fpr);
        if (!key)
            throw PgpError(QStringLiteral("no key for recipient %1").arg(fpr));
        check(rnp_op_encrypt_add_recipient(op, key.get()), "rnp_op_encrypt_add_recipient");
        keys.push_back(std::move(key));
    }
    if (!signer.isEmpty()) {
        Key key = locate(m_ffi, "fingerprint", signer);
        if (!key)
            throw PgpError(QStringLiteral("no signing key %1").arg(signer));
        check(rnp_op_encrypt_add_signature(op, key.get(), nullptr), "rnp_op_encrypt_add_signature");
        keys.push_back(std::move(key));
    }
    check(rnp_op_encrypt_set_armor(op, true), "rnp_op_encrypt_set_armor");
    check(rnp_op_encrypt_set_cipher(op, "AES256"), "rnp_op_encrypt_set_cipher");
    check(rnp_op_encrypt_execute(op), "encrypt");
    return bytesOf(out.get());
}

namespace {

// One OpenPGP packet: where it starts and ends, and its tag. Only definite
// lengths: the session key packets that lead a message never use partial
// ones.
struct Packet
{
    qsizetype start = 0;
    qsizetype body = 0;
    qsizetype end = 0;
    int tag = 0;
};

std::optional<Packet> packetAt(const QByteArray &d, qsizetype pos)
{
    auto byte = [&d](qsizetype i) { return i < d.size() ? int(quint8(d[i])) : -1; };
    const int b = byte(pos);
    if (b < 0 || !(b & 0x80))
        return std::nullopt;
    Packet p;
    p.start = pos;
    qsizetype len = 0;
    qsizetype at = pos + 1;
    if (b & 0x40) { // new format
        p.tag = b & 0x3f;
        const int o = byte(at++);
        if (o < 0 || (o >= 224 && o < 255))
            return std::nullopt;
        if (o < 192) {
            len = o;
        } else if (o < 224) {
            const int o2 = byte(at++);
            if (o2 < 0)
                return std::nullopt;
            len = ((o - 192) << 8) + o2 + 192;
        } else {
            for (int i = 0; i < 4; ++i) {
                const int x = byte(at++);
                if (x < 0)
                    return std::nullopt;
                len = (len << 8) | x;
            }
        }
    } else { // old format
        p.tag = (b >> 2) & 0x0f;
        const int bytes = (b & 3) == 0 ? 1 : (b & 3) == 1 ? 2 : (b & 3) == 2 ? 4 : 0;
        if (!bytes)
            return std::nullopt;
        for (int i = 0; i < bytes; ++i) {
            const int x = byte(at++);
            if (x < 0)
                return std::nullopt;
            len = (len << 8) | x;
        }
    }
    p.body = at;
    p.end = at + len;
    if (p.end > d.size())
        return std::nullopt;
    return p;
}

// RNP 0.18 tries hidden recipients (key id 0, as Delta Chat sends to
// everyone) only in the first public-key session key packet: when ours is
// the second, it gives up. These are the same message with each later
// hidden-recipient packet moved to the front.
QList<QByteArray> hiddenRecipientOrders(const QByteArray &binary)
{
    QList<Packet> esks;
    qsizetype pos = 0;
    while (const auto p = packetAt(binary, pos)) {
        if (p->tag != 1 && p->tag != 3)
            break;
        esks.append(*p);
        pos = p->end;
    }
    // A version 3 public-key session key packet naming key id 0.
    const auto hidden = [&binary](const Packet &p) {
        return p.tag == 1 && p.end - p.body >= 10 && quint8(binary[p.body]) == 3
            && binary.mid(p.body + 1, 8) == QByteArray(8, '\0');
    };
    QList<QByteArray> out;
    for (qsizetype i = 1; i < esks.size(); ++i) {
        if (!hidden(esks[i]))
            continue;
        QByteArray d = binary.mid(esks[i].start, esks[i].end - esks[i].start);
        for (qsizetype j = 0; j < esks.size(); ++j) {
            if (j != i)
                d += binary.mid(esks[j].start, esks[j].end - esks[j].start);
        }
        d += binary.mid(pos);
        out.append(d);
    }
    return out;
}

} // namespace

Pgp::Decrypted Pgp::decrypt(const QByteArray &data) const
{
    try {
        return decryptOnce(data);
    } catch (const PgpError &) {
        QByteArray binary = data;
        if (data.trimmed().startsWith("-----BEGIN PGP MESSAGE-----")) {
            const Input in = inputFrom(data);
            const Output out = memoryOutput();
            if (rnp_dearmor(in.get(), out.get()) != RNP_SUCCESS)
                throw;
            binary = bytesOf(out.get());
        }
        for (const QByteArray &reordered : hiddenRecipientOrders(binary)) {
            try {
                return decryptOnce(reordered);
            } catch (const PgpError &) {
            }
        }
        throw;
    }
}

Pgp::Decrypted Pgp::decryptOnce(const QByteArray &data) const
{
    const Input in = inputFrom(data);
    const Output out = memoryOutput();
    rnp_op_verify_t op = nullptr;
    check(rnp_op_verify_create(&op, m_ffi, in.get(), out.get()), "rnp_op_verify_create");
    const std::unique_ptr<rnp_op_verify_st, decltype(&rnp_op_verify_destroy)> guard(op, &rnp_op_verify_destroy);
#ifdef RNP_VERIFY_ALLOW_HIDDEN_RECIPIENT
    // Delta Chat hides who a message is for: every session key packet names
    // key id 0. RNP then tries each secret key, but only when asked to.
    check(rnp_op_verify_set_flags(op, RNP_VERIFY_ALLOW_HIDDEN_RECIPIENT), "rnp_op_verify_set_flags");
#endif
    const rnp_result_t rc = rnp_op_verify_execute(op);

    Decrypted result;
    char *mode = nullptr;
    char *cipher = nullptr;
    bool valid = false;
    if (rnp_op_verify_get_protection_info(op, &mode, &cipher, &valid) == RNP_SUCCESS) {
        const QString m = takeString(mode);
        rnp_buffer_destroy(cipher);
        result.encrypted = m != QLatin1String("none") && valid;
    }
    // A bad signature still yields plaintext; only a decryption failure is fatal.
    if (rc != RNP_SUCCESS && rc != RNP_ERROR_SIGNATURE_INVALID && rc != RNP_ERROR_KEY_NOT_FOUND
        && rc != RNP_ERROR_SIGNATURE_EXPIRED)
        check(rc, "decrypt");
    if (rc == RNP_ERROR_KEY_NOT_FOUND && !result.encrypted)
        check(rc, "decrypt"); // it was the decryption key that was missing

    result.data = bytesOf(out.get());
    signersOf(op, result.validSigners, result.invalidSigners);
    return result;
}

QByteArray Pgp::signDetached(const QByteArray &data, const QString &signer) const
{
    const Input in = inputFrom(data);
    const Output out = memoryOutput();
    rnp_op_sign_t op = nullptr;
    check(rnp_op_sign_detached_create(&op, m_ffi, in.get(), out.get()), "rnp_op_sign_detached_create");
    const std::unique_ptr<rnp_op_sign_st, decltype(&rnp_op_sign_destroy)> guard(op, &rnp_op_sign_destroy);
    const Key key = locate(m_ffi, "fingerprint", signer);
    if (!key)
        throw PgpError(QStringLiteral("no signing key %1").arg(signer));
    check(rnp_op_sign_add_signature(op, key.get(), nullptr), "rnp_op_sign_add_signature");
    check(rnp_op_sign_set_armor(op, true), "rnp_op_sign_set_armor");
    check(rnp_op_sign_execute(op), "sign");
    return bytesOf(out.get());
}

QByteArray Pgp::signCleartext(const QByteArray &text, const QString &signer) const
{
    const Input in = inputFrom(text);
    const Output out = memoryOutput();
    rnp_op_sign_t op = nullptr;
    check(rnp_op_sign_cleartext_create(&op, m_ffi, in.get(), out.get()), "rnp_op_sign_cleartext_create");
    const std::unique_ptr<rnp_op_sign_st, decltype(&rnp_op_sign_destroy)> guard(op, &rnp_op_sign_destroy);
    const Key key = locate(m_ffi, "fingerprint", signer);
    if (!key)
        throw PgpError(QStringLiteral("no signing key %1").arg(signer));
    check(rnp_op_sign_add_signature(op, key.get(), nullptr), "rnp_op_sign_add_signature");
    check(rnp_op_sign_execute(op), "sign");
    return bytesOf(out.get());
}

QStringList Pgp::verifyDetached(const QByteArray &data, const QByteArray &signature) const
{
    const Input in = inputFrom(data);
    const Input sig = inputFrom(signature);
    rnp_op_verify_t op = nullptr;
    check(rnp_op_verify_detached_create(&op, m_ffi, in.get(), sig.get()), "rnp_op_verify_detached_create");
    const std::unique_ptr<rnp_op_verify_st, decltype(&rnp_op_verify_destroy)> guard(op, &rnp_op_verify_destroy);
    const rnp_result_t rc = rnp_op_verify_execute(op);
    // A bad or unknown signature is an answer, not a failure.
    if (rc != RNP_SUCCESS && rc != RNP_ERROR_SIGNATURE_INVALID && rc != RNP_ERROR_KEY_NOT_FOUND
        && rc != RNP_ERROR_SIGNATURE_EXPIRED)
        check(rc, "verify");
    QStringList valid, invalid;
    signersOf(op, valid, invalid);
    return valid;
}

} // namespace e3::crypto
