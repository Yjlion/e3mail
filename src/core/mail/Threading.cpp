// SPDX-License-Identifier: MPL-2.0
#include "Threading.h"

#include "store/Database.h"

#include <QSet>

namespace e3::mail {

qint64 Threading::assign(MailContext &ctx, qint64 msgId, const QString &messageId, const QString &inReplyTo,
                         const QStringList &references, qint64 date)
{
    QStringList ids = references;
    if (!inReplyTo.isEmpty())
        ids.append(inReplyTo);
    ids.append(messageId);
    ids.removeDuplicates();
    ids.removeAll(QString());

    QList<qint64> threads;
    for (const QString &id : std::as_const(ids)) {
        if (const auto t = ctx.db.queryInt("SELECT thread_id FROM thread_refs WHERE message_id=?", id)) {
            if (!threads.contains(*t))
                threads.append(*t);
        }
    }
    qint64 thread;
    if (threads.isEmpty()) {
        ctx.db.run("INSERT INTO threads(last_activity) VALUES(?)", date);
        thread = ctx.db.lastInsertId();
    } else {
        std::sort(threads.begin(), threads.end());
        thread = threads.first();
        // This message bridges threads that turn out to be one conversation.
        for (qsizetype i = 1; i < threads.size(); ++i) {
            ctx.db.run("UPDATE messages SET thread_id=? WHERE thread_id=?", thread, threads[i]);
            ctx.db.run("UPDATE thread_refs SET thread_id=? WHERE thread_id=?", thread, threads[i]);
            ctx.db.run("DELETE FROM threads WHERE id=?", threads[i]);
        }
    }
    for (const QString &id : std::as_const(ids))
        ctx.db.run("INSERT OR REPLACE INTO thread_refs(message_id, thread_id) VALUES(?, ?)", id, thread);
    ctx.db.run("UPDATE messages SET thread_id=? WHERE id=?", thread, msgId);
    ctx.db.run("UPDATE threads SET last_activity=max(last_activity, ?) WHERE id=?", date, thread);
    return thread;
}

} // namespace e3::mail
