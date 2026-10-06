// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Types.h"

#include <optional>

namespace e3::mail {

class Contacts
{
public:
    // Creates or updates a contact, only ever raising its origin. A name is
    // recorded only when none is set, or when the user sets it (Manual).
    static qint64 touch(MailContext &ctx, const QString &addr, const QString &name, ContactOrigin origin);

    static std::optional<ContactInfo> get(MailContext &ctx, const QString &addr);
    static std::optional<ContactInfo> byId(MailContext &ctx, qint64 id);
    // Address book listing: everyone, matched on name or address.
    static QList<ContactInfo> list(MailContext &ctx, const QString &filter = {}, int limit = 2000);

    // Is mail from this address trusted enough for the Inbox? Verified, known,
    // or ourselves.
    static bool isTrusted(MailContext &ctx, const QString &addr);

    // Adopts a key for a contact. `seenAt` is the Date of the message that
    // carried it; an older message never replaces a newer key, and a verified
    // contact's key is never replaced by an unauthenticated one.
    static bool adoptKey(MailContext &ctx, const QString &addr, const QString &fpr, bool preferEncrypt,
                         const QDateTime &seenAt, bool fromGossip);

    static void setEncryptionOverride(MailContext &ctx, qint64 contactId, std::optional<EncryptionMode> mode);
    static void setName(MailContext &ctx, qint64 contactId, const QString &name);
    static void setVerified(MailContext &ctx, const QString &addr, const QString &fpr);

    // Blocklist patterns: "someone@example.com", or "@example.com" for exactly
    // that domain (not its subdomains).
    static void block(MailContext &ctx, const QString &pattern);
    static void unblock(MailContext &ctx, const QString &pattern);
    static QStringList blocklist(MailContext &ctx);
    static bool isBlocked(MailContext &ctx, const QString &addr);
    static QString normalizePattern(const QString &pattern);
};

} // namespace e3::mail
