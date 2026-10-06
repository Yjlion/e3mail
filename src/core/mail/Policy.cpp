// SPDX-License-Identifier: MPL-2.0
#include "Policy.h"

#include "Contacts.h"
#include "Keyring.h"
#include "crypto/Pgp.h"
#include "store/Config.h"

namespace e3::mail {

Policy::Readiness Policy::evaluate(MailContext &ctx, const QStringList &recipients, SendEncryption padlock)
{
    Readiness r;
    auto mode = EncryptionMode(qBound(0, ctx.config.getInt(cfg::EncryptionMode), 2));
    bool allPreferEncrypt = true;
    QStringList seen;
    for (const QString &raw : recipients) {
        const QString addr = mime::normalizeAddr(raw);
        if (addr.isEmpty() || seen.contains(addr))
            continue;
        seen.append(addr);
        const auto c = Contacts::get(ctx, addr);
        if (c && c->encryptionOverride && int(*c->encryptionOverride) > int(mode))
            mode = *c->encryptionOverride;
        if (c && !c->fingerprint.isEmpty() && Keyring::ensure(ctx, c->fingerprint)
            && ctx.pgp.canEncryptTo(c->fingerprint)) {
            r.recipientFprs.append(c->fingerprint);
            allPreferEncrypt = allPreferEncrypt && c->preferEncrypt;
        } else {
            r.missingKeys.append(addr);
        }
    }
    r.effectiveMode = mode;
    r.padlockLocked = mode == EncryptionMode::Strict;
    const bool allKeys = r.missingKeys.isEmpty() && !seen.isEmpty();

    switch (padlock) {
    case SendEncryption::Required:
        r.willEncrypt = true;
        if (!allKeys) {
            r.canSend = false;
            r.refusal = seen.isEmpty() ? QStringLiteral("Add a recipient.")
                                       : QStringLiteral("No key for %1.").arg(r.missingKeys.join(QStringLiteral(", ")));
        }
        break;
    case SendEncryption::Plaintext:
        if (mode == EncryptionMode::Strict) {
            r.canSend = false;
            r.willEncrypt = true;
            r.refusal = QStringLiteral("Encryption is required for these recipients.");
        }
        break;
    case SendEncryption::Auto:
        if (mode == EncryptionMode::Strict) {
            r.willEncrypt = true;
            if (!allKeys) {
                r.canSend = false;
                r.refusal = seen.isEmpty() ? QStringLiteral("Add a recipient.")
                                           : QStringLiteral("Encryption is required, and there is no key for %1.")
                                                 .arg(r.missingKeys.join(QStringLiteral(", ")));
            }
        } else if (mode == EncryptionMode::Opportunistic) {
            r.willEncrypt = allKeys;
        } else {
            r.willEncrypt = allKeys && allPreferEncrypt;
        }
        break;
    }
    if (!r.willEncrypt)
        r.recipientFprs.clear();
    return r;
}

} // namespace e3::mail
