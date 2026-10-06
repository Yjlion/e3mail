// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Types.h"

namespace e3::mail {

// Decides whether a message is encrypted.
//
// Opportunistic means: encrypt when *every* recipient has a key, otherwise
// send cleartext to everyone. Autocrypt recommends exactly this, and it
// removes the failure where some recipients silently do not get an encrypted
// message because their key was missing. Under strict mode a missing key
// refuses the send; under lenient, mail is encrypted only when every recipient
// also asked for it (prefer-encrypt=mutual).
//
// Per-contact overrides and the global mode compose toward the strictest. The
// composer's padlock can demand encryption, or ask for cleartext where no
// applicable setting says end-to-end only.
class Policy
{
public:
    struct Readiness
    {
        EncryptionMode effectiveMode = EncryptionMode::Opportunistic;
        bool willEncrypt = false;
        bool canSend = true;
        QString refusal;            // why not, when canSend is false
        QStringList missingKeys;    // recipients without a usable key
        QStringList recipientFprs;  // keys to encrypt to, when encrypting
        bool padlockLocked = false; // the policy forbids cleartext
    };

    static Readiness evaluate(MailContext &ctx, const QStringList &recipients, SendEncryption padlock);
};

} // namespace e3::mail
