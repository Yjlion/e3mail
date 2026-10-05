// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Types.h"

#include <optional>

namespace e3::mail {

// Read side of the mailbox: every list view is one SearchQuery.
class Search
{
public:
    static QList<MessageSummary> list(MailContext &ctx, const SearchQuery &q);
    static std::optional<MessageDetail> detail(MailContext &ctx, qint64 msgId);
    // The conversation a message belongs to, oldest first. Trashed messages
    // appear only when the message asked about is itself trashed.
    static QList<qint64> thread(MailContext &ctx, qint64 msgId);
    static int count(MailContext &ctx, const QString &tag, bool unreadOnly);

    // Indexes a message for full-text search.
    static void index(MailContext &ctx, qint64 msgId);

    // Builds the WHERE clause for a system tag, over messages aliased "m".
    static QByteArray tagClause(const QString &tag);
};

} // namespace e3::mail
