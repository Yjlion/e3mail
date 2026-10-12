// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "mail/Types.h"

#include <QSet>

namespace e3::sync {

// Ops through the mailbox, for devices that are never online together
// (ADR 0010, amended 2026-10-11). A device mails its own ops to its own
// address, encrypted to the account's key and signed with it, under
// `X-E3-Sync` (stored hidden, never shown). Each mail also says what the
// sender holds of everyone's ops, which is how the others learn what to
// send next. The mail is an ordinary server message: deletion waits for
// every device, as for any other.
class MailboxFallback
{
public:
    static constexpr int kBatch = 500;
    static constexpr char kHeader[] = "X-E3-Sync";

    // Queues one sync mail if a paired device that is not connected lacks
    // this device's ops, or the rest of a full batch. Returns the message
    // id, or 0.
    static qint64 queueIfDue(MailContext &ctx, const QSet<QString> &connected);

    // A sync mail, decrypted: applies its ops if this account's own key
    // signed it, and remembers what its sender holds. True if it was one.
    static bool receive(MailContext &ctx, const QString &body, const QStringList &validSigners);
};

} // namespace e3::sync
