// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Types.h"

namespace e3::mail {

// Everything that moves mail between system tags, plus user labels, plus the
// housekeeping that enforces deadlines. Trash is the only place that destroys
// mail on a timer; every other route out of view goes through it first.
class Organize
{
public:
    static void markRead(MailContext &ctx, qint64 msgId, bool read);

    static void trash(MailContext &ctx, qint64 msgId, TrashReason reason);
    static void restore(MailContext &ctx, qint64 msgId);
    // Destroys now: the same path the purge deadline takes.
    static void purge(MailContext &ctx, qint64 msgId);
    static int emptyTrash(MailContext &ctx);

    static void archive(MailContext &ctx, qint64 msgId, bool archived);

    // Marks the message as waiting in Unverified.
    static void hold(MailContext &ctx, qint64 msgId);
    // Accepts a sender: they become known and all their held mail is released.
    static int accept(MailContext &ctx, const QString &addr);
    // Releases held mail from an address that is now trusted (e.g. we wrote to
    // them). Returns how many were released.
    static int releaseHeld(MailContext &ctx, const QString &addr);

    // User labels.
    static QList<LabelInfo> labels(MailContext &ctx);
    static qint64 createLabel(MailContext &ctx, const QString &name, const QString &color);
    static bool renameLabel(MailContext &ctx, qint64 labelId, const QString &name, const QString &color);
    static bool deleteLabel(MailContext &ctx, qint64 labelId);
    static void setLabel(MailContext &ctx, qint64 msgId, qint64 labelId, bool on);
    static qint64 archiveLabelId(MailContext &ctx);

    struct HousekeepingResult
    {
        int purged = 0;
        int swept = 0;
        int rawExpired = 0;
        int blobsRemoved = 0;
    };
    static HousekeepingResult housekeeping(MailContext &ctx);

    static QString messageIdOf(MailContext &ctx, qint64 msgId);
};

} // namespace e3::mail
