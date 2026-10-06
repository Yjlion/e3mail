// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "mail/Types.h"
#include "store/OpLog.h"

#include <QHash>
#include <QList>
#include <QStringList>

namespace e3::sync {

// Replays other devices' ops into this mailbox, by the rules of ADR 0013:
// the log decides which op of a merge key wins, purge is a tombstone, and an
// op for a message not stored yet waits in the log until it is.
class OpApply
{
public:
    // Highest sequence number held from each device, this one included.
    using Versions = QHash<QString, qint64>;

    struct Result
    {
        int accepted = 0;      // new ops added to the log
        bool gap = false;      // some ops were out of sequence and dropped
        bool changed = false;  // the mailbox visibly changed
        QStringList wantedRaw; // msg.add whose raw message is not here: blob names
    };

    // Ops must be in sequence per device. Duplicates are skipped, and a gap
    // stops that device's ops until the next exchange.
    static Result apply(MailContext &ctx, const QList<Op> &ops);

    // Applies, for a message just stored, the newest op of each of its merge
    // keys. Called from inside Ingest's transaction.
    static void replayFor(MailContext &ctx, const QString &messageId);

    static Versions versions(Database &db);
    // Ops the holder of `have` lacks, oldest first per device.
    static QList<Op> missing(Database &db, const Versions &have, int limit);

    static bool isTombstoned(Database &db, const QString &messageId);
};

} // namespace e3::sync
