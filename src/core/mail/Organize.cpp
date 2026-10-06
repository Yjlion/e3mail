// SPDX-License-Identifier: MPL-2.0
#include "Organize.h"

#include "Contacts.h"
#include "store/BlobStore.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"
#include "util/Log.h"

#include <QJsonObject>
#include <QSet>

namespace e3::mail {

namespace {
constexpr qint64 kDayMs = 24LL * 3600 * 1000;

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QJsonObject msgRef(MailContext &ctx, qint64 msgId)
{
    return {{QStringLiteral("mid"), Organize::messageIdOf(ctx, msgId)}};
}
} // namespace

QString Organize::messageIdOf(MailContext &ctx, qint64 msgId)
{
    return ctx.db.queryText("SELECT message_id FROM messages WHERE id=?", msgId).value_or(QString());
}

void Organize::markRead(MailContext &ctx, qint64 msgId, bool read)
{
    Transaction tx(ctx.db);
    ctx.db.run("UPDATE messages SET unread=? WHERE id=?", !read, msgId);
    if (ctx.db.changes()) {
        QJsonObject p = msgRef(ctx, msgId);
        p.insert(QStringLiteral("read"), read);
        ctx.ops.record(op::ReadState, p);
    }
    tx.commit();
}

void Organize::trash(MailContext &ctx, qint64 msgId, TrashReason reason)
{
    const qint64 days = ctx.config.getInt(cfg::TrashPurgeDays);
    const qint64 now = nowMs();
    ctx.db.run("INSERT OR IGNORE INTO trashed(msg_id, trashed_at, purge_at, reason) VALUES(?, ?, ?, ?)", msgId, now,
               now + qMax<qint64>(0, days) * kDayMs, int(reason));
    // Placement by rule (blocked, cleartext under strict, never accepted) is
    // derived from replicated state, so every device reaches it alone; only
    // what a person or a timer did travels (ADR 0013).
    const bool derived =
        reason == TrashReason::Blocked || reason == TrashReason::Cleartext || reason == TrashReason::Unaccepted;
    if (ctx.db.changes() && !derived) {
        QJsonObject p = msgRef(ctx, msgId);
        p.insert(QStringLiteral("reason"), int(reason));
        ctx.ops.record(op::Trash, p);
    }
}

void Organize::restore(MailContext &ctx, qint64 msgId)
{
    Transaction tx(ctx.db);
    const auto reason = ctx.db.queryInt("SELECT reason FROM trashed WHERE msg_id=?", msgId);
    ctx.db.run("DELETE FROM trashed WHERE msg_id=?", msgId);
    // Restoring mail that was swept out of Unverified is accepting its sender;
    // otherwise it would be swept straight back.
    if (reason && TrashReason(*reason) == TrashReason::Unaccepted) {
        if (const auto from = ctx.db.queryText("SELECT from_addr FROM messages WHERE id=?", msgId))
            accept(ctx, *from);
    }
    ctx.db.run("DELETE FROM held WHERE msg_id=?", msgId);
    ctx.ops.record(op::Restore, msgRef(ctx, msgId));
    tx.commit();
}

void Organize::purge(MailContext &ctx, qint64 msgId)
{
    const QString mid = messageIdOf(ctx, msgId);
    ctx.ops.record(op::Purge, {{QStringLiteral("mid"), mid}});
    // A tombstone, so neither another device nor the server brings it back.
    ctx.db.run("INSERT OR IGNORE INTO tombstones(message_id, purged_at) VALUES(?, ?)", mid, nowMs());
    ctx.db.run("DELETE FROM msg_fts WHERE rowid=?", msgId);
    ctx.db.run("DELETE FROM messages WHERE id=?", msgId);
}

int Organize::emptyTrash(MailContext &ctx)
{
    Transaction tx(ctx.db);
    QList<qint64> ids;
    Statement st(ctx.db, "SELECT msg_id FROM trashed");
    while (st.step())
        ids.append(st.int64(0));
    for (qint64 id : std::as_const(ids))
        purge(ctx, id);
    tx.commit();
    return int(ids.size());
}

qint64 Organize::archiveLabelId(MailContext &ctx)
{
    return ctx.db.queryInt("SELECT id FROM labels WHERE name_norm='archive' AND system=1").value_or(0);
}

void Organize::archive(MailContext &ctx, qint64 msgId, bool archived)
{
    setLabel(ctx, msgId, archiveLabelId(ctx), archived);
}

void Organize::hold(MailContext &ctx, qint64 msgId)
{
    ctx.db.run("INSERT OR IGNORE INTO held(msg_id, held_at) VALUES(?, ?)", msgId, nowMs());
}

int Organize::accept(MailContext &ctx, const QString &addr)
{
    Contacts::touch(ctx, addr, QString(), ContactOrigin::Manual);
    ctx.ops.record(op::Accept, {{QStringLiteral("addr"), mime::normalizeAddr(addr)}});
    return releaseHeld(ctx, addr);
}

int Organize::releaseHeld(MailContext &ctx, const QString &addr)
{
    ctx.db.run("DELETE FROM held WHERE msg_id IN (SELECT id FROM messages WHERE lower(from_addr)=?)",
               mime::normalizeAddr(addr));
    return ctx.db.changes();
}

QList<LabelInfo> Organize::labels(MailContext &ctx)
{
    QList<LabelInfo> out;
    Statement st(ctx.db, "SELECT l.id, l.name, l.color, l.system, "
                         "(SELECT count(*) FROM msg_labels ml WHERE ml.label_id = l.id "
                         " AND ml.msg_id NOT IN (SELECT msg_id FROM trashed)) "
                         "FROM labels l WHERE l.system = 0 ORDER BY l.name_norm");
    while (st.step())
        out.append({st.int64(0), st.text(1), st.text(2), bool(st.integer(3)), st.integer(4)});
    return out;
}

qint64 Organize::createLabel(MailContext &ctx, const QString &name, const QString &color)
{
    const QString trimmed = name.trimmed();
    const QString norm = trimmed.toLower();
    if (trimmed.isEmpty())
        return 0;
    if (const auto existing = ctx.db.queryInt("SELECT id FROM labels WHERE name_norm=?", norm))
        return *existing;
    ctx.db.run("INSERT INTO labels(name, name_norm, color) VALUES(?, ?, ?)", trimmed, norm, color);
    const qint64 id = ctx.db.lastInsertId();
    ctx.ops.record(op::LabelDefine, {{QStringLiteral("name"), trimmed}, {QStringLiteral("color"), color}});
    return id;
}

bool Organize::renameLabel(MailContext &ctx, qint64 labelId, const QString &name, const QString &color)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return false;
    const auto old = ctx.db.queryText("SELECT name FROM labels WHERE id=? AND system=0", labelId);
    if (!old)
        return false;
    ctx.db.run("UPDATE labels SET name=?, name_norm=?, color=? WHERE id=? AND system=0", trimmed, trimmed.toLower(),
               color, labelId);
    ctx.ops.record(op::LabelRename,
                   {{QStringLiteral("name"), *old}, {QStringLiteral("to"), trimmed}, {QStringLiteral("color"), color}});
    return true;
}

bool Organize::deleteLabel(MailContext &ctx, qint64 labelId)
{
    const auto name = ctx.db.queryText("SELECT name FROM labels WHERE id=? AND system=0", labelId);
    if (!name)
        return false;
    ctx.db.run("DELETE FROM labels WHERE id=? AND system=0", labelId);
    ctx.ops.record(op::LabelDelete, {{QStringLiteral("name"), *name}});
    return true;
}

void Organize::setLabel(MailContext &ctx, qint64 msgId, qint64 labelId, bool on)
{
    if (!labelId)
        return;
    if (on)
        ctx.db.run("INSERT OR IGNORE INTO msg_labels(msg_id, label_id) VALUES(?, ?)", msgId, labelId);
    else
        ctx.db.run("DELETE FROM msg_labels WHERE msg_id=? AND label_id=?", msgId, labelId);
    if (ctx.db.changes()) {
        QJsonObject p = msgRef(ctx, msgId);
        p.insert(QStringLiteral("label"),
                 ctx.db.queryText("SELECT name FROM labels WHERE id=?", labelId).value_or(QString()));
        ctx.ops.record(on ? op::LabelAdd : op::LabelRemove, p);
    }
}

Organize::HousekeepingResult Organize::housekeeping(MailContext &ctx)
{
    HousekeepingResult r;
    const qint64 now = nowMs();
    Transaction tx(ctx.db);

    // Unverified mail past its window goes to Trash. The deadline is read
    // from the setting now, not stored, so changing the setting applies to
    // mail already waiting. 0 means never sweep.
    if (const int days = ctx.config.getInt(cfg::UnverifiedTrashDays); days > 0) {
        QList<qint64> due;
        Statement st(ctx.db, "SELECT msg_id FROM held WHERE held_at <= ? "
                             "AND msg_id NOT IN (SELECT msg_id FROM trashed)");
        st.bind(1, now - days * kDayMs);
        while (st.step())
            due.append(st.int64(0));
        for (qint64 id : std::as_const(due)) {
            trash(ctx, id, TrashReason::Unaccepted);
            ++r.swept;
        }
    }

    QList<qint64> purgeable;
    {
        Statement st(ctx.db, "SELECT msg_id FROM trashed WHERE purge_at <= ?");
        st.bind(1, now);
        while (st.step())
            purgeable.append(st.int64(0));
    }
    for (qint64 id : std::as_const(purgeable))
        purge(ctx, id);
    r.purged = int(purgeable.size());

    ctx.db.run("UPDATE messages SET raw_blob=NULL, raw_expires_at=NULL "
               "WHERE raw_blob IS NOT NULL AND raw_expires_at IS NOT NULL AND raw_expires_at <= ?",
               now);
    r.rawExpired = ctx.db.changes();
    ctx.db.run("DELETE FROM threads WHERE id NOT IN (SELECT DISTINCT thread_id FROM messages WHERE thread_id IS NOT NULL)");
    tx.commit();

    // Blobs referenced by nothing are reclaimed in the same pass that freed them.
    QSet<QString> referenced;
    Statement st(ctx.db, "SELECT raw_blob FROM messages WHERE raw_blob IS NOT NULL "
                         "UNION SELECT blob FROM attachments");
    while (st.step())
        referenced.insert(st.text(0));
    r.blobsRemoved = ctx.blobs.collectGarbage(referenced);
    if (r.purged || r.swept || r.rawExpired)
        qCInfo(lcMail) << "housekeeping: purged" << r.purged << "swept" << r.swept << "raw expired" << r.rawExpired;
    return r;
}

} // namespace e3::mail
