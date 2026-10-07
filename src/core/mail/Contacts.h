// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Types.h"
#include "VCard.h"

#include <QJsonObject>

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

    // The address book (P10). Each opens its own transaction and records its
    // op in it.
    //
    // Adds someone by hand, with origin Manual: their mail is trusted from
    // then on. Nothing when the address is not one or is in the book already;
    // someone only seen on mail is taken into the book.
    static std::optional<qint64> create(MailContext &ctx, const QString &addr, const QString &name);
    // Name, organization, title, notes, birthday and phones, from `d`. An
    // unreadable birthday is dropped; phones without a number are skipped.
    static void setDetails(MailContext &ctx, qint64 contactId, const ContactInfo &d);
    // Out of the address book. Their mail waits in Unverified again; their
    // key stays in the keyring, and new mail from them makes them a contact
    // that was only seen.
    static void remove(MailContext &ctx, qint64 contactId);

    struct ImportResult
    {
        int added = 0;
        int updated = 0;
        int skipped = 0; // cards without an email address
    };
    // One contact per address on a card. A contact already here only gets
    // the fields it lacks; nothing it has is overwritten. Everyone imported
    // is in the address book, so their mail is trusted.
    static ImportResult import(MailContext &ctx, const QList<VCard::Card> &cards);

    // For ops from other devices: without a transaction or an op.
    static void applyDetails(MailContext &ctx, const QString &addr, const QJsonObject &payload);

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
