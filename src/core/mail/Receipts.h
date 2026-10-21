// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Types.h"

#include "mime/Part.h"

namespace e3::mail {

// Read receipts (RFC 8098, ADR 0022). A sender asks with
// Disposition-Notification-To; reading their message answers with a hidden
// multipart/report, once across all devices, only to someone trusted and
// only while the setting allows. Receipts that come back for our own mail
// are recorded per reader.
class Receipts
{
public:
    // Where `headers` ask a receipt to go: Disposition-Notification-To, or
    // Delta Chat's Chat-Disposition-Notification-To. Empty when not asked.
    static QString requestedBy(const mime::HeaderList &headers);

    // After a message was marked read by the person: sends its receipt if it
    // asked for one and every condition holds. Inside the caller's
    // transaction. Returns whether one was queued.
    static bool sendIfDue(MailContext &ctx, qint64 msgId);

    // Is this a disposition-notification report?
    static bool isReport(const mime::Part &content);
    // Records a receipt for our own message it names, if `from` was one of
    // its recipients. Inside the caller's transaction.
    static bool receive(MailContext &ctx, const mime::Part &content, const QString &from, const QDateTime &date);
};

} // namespace e3::mail
