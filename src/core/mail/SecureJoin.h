// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Types.h"

#include "mime/Headers.h"

#include <optional>

namespace e3::mail {

// SecureJoin's setup-contact protocol, as Delta Chat speaks it (ADR 0021):
// two people verify each other's keys by one scanning the other's invite.
//
// Alice shows an invite: her fingerprint, address, and two random tokens.
// Bob scans it and writes to her (vc-request, cleartext, carrying his key).
// She answers encrypted (vc-auth-required). If her key is the one the invite
// named, Bob verifies her and sends the second token back encrypted
// (vc-request-with-auth); if the token is hers and his signature is by the
// key he names, Alice verifies him and confirms (vc-contact-confirm).
//
// Every handshake message is stored hidden and never shown. Invites and
// joins travel to the account's other devices as ops, so whichever device
// fetches the next step can answer it.
class SecureJoin
{
public:
    struct Invite
    {
        QString fpr;          // the inviter's key, normalised
        QString addr;
        QString name;
        QString invitenumber; // asks for the inviter's key
        QString authcode;     // proves the joiner saw the invite
    };

    enum class JoinState { None = 0, Requested = 1, AuthSent = 2, Done = 3, Failed = -1 };
    struct Join
    {
        QString addr;
        QString name;
        QString fpr;
        JoinState state = JoinState::None;
        QString error;
        qint64 startedAt = 0;
    };

    // Joins are given up after this long without an answer.
    static constexpr qint64 kJoinTimeoutMs = 7LL * 24 * 3600 * 1000;
    // Handshake messages older than this are not answered: a device that
    // fetches an old one late must not start a conversation nobody waits for.
    static constexpr qint64 kAnswerWithinMs = 7LL * 24 * 3600 * 1000;

    // This account's invite, as `OPENPGP4FPR:` text for a QR code. The same
    // one each time until `renewInvite`. Creates the account key if needed.
    static QString invite(MailContext &ctx);
    // Revokes every earlier invite: codes already shown stop working.
    static QString renewInvite(MailContext &ctx);

    // `OPENPGP4FPR:` invites, and Delta Chat's `https://i.delta.chat/#` ones.
    static std::optional<Invite> parse(const QString &text);
    static QString format(const Invite &invite);

    // Starts verifying the person who made `invite`. Throws ComposeError
    // with a reason a person can read when it cannot start.
    static void join(MailContext &ctx, const Invite &invite);

    static std::optional<Join> joinFor(MailContext &ctx, const QString &addr);
    static QList<Join> joins(MailContext &ctx);

    // Is this a handshake message? `headers` are the merged ones (protected
    // inner headers win).
    static bool isHandshake(const mime::HeaderList &headers);

    struct Incoming
    {
        mime::HeaderList headers;  // merged
        QList<QByteArray> gossip;  // Autocrypt-Gossip inside the encryption
        QString from;
        bool encrypted = false;
        QStringList validSigners;
        QDateTime date;
    };
    // Takes the next step for a handshake message, inside Ingest's
    // transaction, after the keys it carries were learned. Returns whether
    // anything was done.
    static bool receive(MailContext &ctx, const Incoming &msg);

    // Gives up joins that waited too long. Returns how many.
    static int expire(MailContext &ctx, qint64 now);

    // For ops from other devices.
    static void applyInvite(MailContext &ctx, const QJsonObject &payload);
    static void applyJoin(MailContext &ctx, const QJsonObject &payload);
};

} // namespace e3::mail
