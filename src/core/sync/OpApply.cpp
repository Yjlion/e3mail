// SPDX-License-Identifier: MPL-2.0
#include "OpApply.h"

#include "mail/Contacts.h"
#include "mail/Organize.h"
#include "mail/Preferences.h"
#include "mime/Headers.h"
#include "store/BlobStore.h"
#include "store/Config.h"
#include "store/Database.h"
#include "util/Hlc.h"
#include "util/Log.h"

#include <QJsonDocument>
#include <QSet>

namespace e3::sync {

namespace {

constexpr qint64 kDayMs = 24LL * 3600 * 1000;

QString str(const Op &o, const char *field)
{
    return o.payload.value(QLatin1String(field)).toString();
}

std::optional<qint64> localId(MailContext &ctx, const QString &mid)
{
    return ctx.db.queryInt("SELECT id FROM messages WHERE message_id=?", mid);
}

Op readOp(Statement &st)
{
    Op o;
    o.device = st.text(0);
    o.seq = st.int64(1);
    o.hlc = st.int64(2);
    o.kind = st.text(3);
    o.payload = QJsonDocument::fromJson(st.text(4).toUtf8()).object();
    return o;
}

constexpr char kOpColumns[] = "SELECT device, seq, hlc, kind, payload FROM ops ";

std::optional<Op> winner(MailContext &ctx, const QString &mkey)
{
    Statement st(ctx.db, (QByteArray(kOpColumns) + "WHERE mkey=? ORDER BY hlc DESC, device DESC LIMIT 1").constData());
    st.bind(1, mkey);
    if (!st.step())
        return std::nullopt;
    return readOp(st);
}

qint64 labelIdFor(MailContext &ctx, const QString &name)
{
    const QString trimmed = name.trimmed();
    const QString norm = trimmed.toLower();
    ctx.db.run("INSERT OR IGNORE INTO labels(name, name_norm) VALUES(?, ?)", trimmed, norm);
    return ctx.db.queryInt("SELECT id FROM labels WHERE name_norm=?", norm).value_or(0);
}

// Applies one op that has already won its merge key. Never records: the op is
// in the log already. Returns whether anything visible changed.
bool applyOne(MailContext &ctx, const Op &o, OpApply::Result *r)
{
    const QString mid = str(o, "mid");
    if (o.kind == op::MessageAdded) {
        const QString raw = str(o, "raw");
        if (localId(ctx, mid) || OpApply::isTombstoned(ctx.db, mid) || raw.isEmpty())
            return false;
        if (r)
            r->wantedRaw.append(raw);
        return false;
    }
    if (o.kind == op::Purge) {
        ctx.db.run("INSERT OR IGNORE INTO tombstones(message_id, purged_at) VALUES(?, ?)", mid,
                   Hlc::physicalMs(o.hlc));
        const auto id = localId(ctx, mid);
        if (!id)
            return false;
        ctx.db.run("DELETE FROM msg_fts WHERE rowid=?", *id);
        ctx.db.run("DELETE FROM messages WHERE id=?", *id);
        return true;
    }
    if (o.kind == op::ReadState || o.kind == op::Trash || o.kind == op::Restore || o.kind == op::LabelAdd
        || o.kind == op::LabelRemove) {
        const auto id = localId(ctx, mid);
        if (!id)
            return false; // deferred: replayFor() applies it when the message arrives
        if (o.kind == op::ReadState) {
            ctx.db.run("UPDATE messages SET unread=? WHERE id=?", !o.payload.value(QLatin1String("read")).toBool(),
                       *id);
        } else if (o.kind == op::Trash) {
            // The deadline runs from when it was trashed, on whichever device.
            const qint64 at = Hlc::physicalMs(o.hlc);
            const qint64 days = qMax<qint64>(0, ctx.config.getInt(cfg::TrashPurgeDays));
            ctx.db.run("INSERT OR IGNORE INTO trashed(msg_id, trashed_at, purge_at, reason) VALUES(?, ?, ?, ?)", *id,
                       at, at + days * kDayMs, o.payload.value(QLatin1String("reason")).toInt());
        } else if (o.kind == op::Restore) {
            ctx.db.run("DELETE FROM trashed WHERE msg_id=?", *id);
            ctx.db.run("DELETE FROM held WHERE msg_id=?", *id);
        } else if (o.kind == op::LabelAdd) {
            if (const qint64 label = labelIdFor(ctx, str(o, "label")))
                ctx.db.run("INSERT OR IGNORE INTO msg_labels(msg_id, label_id) VALUES(?, ?)", *id, label);
        } else {
            ctx.db.run("DELETE FROM msg_labels WHERE msg_id=? AND label_id="
                       "(SELECT id FROM labels WHERE name_norm=?)",
                       *id, str(o, "label").trimmed().toLower());
        }
        return true;
    }
    if (o.kind == op::LabelDefine) {
        const qint64 id = labelIdFor(ctx, str(o, "name"));
        ctx.db.run("UPDATE labels SET color=? WHERE id=? AND system=0", str(o, "color"), id);
        return true;
    }
    if (o.kind == op::LabelRename) {
        const QString to = str(o, "to").trimmed();
        if (to.isEmpty())
            return false;
        const QString from = str(o, "name").trimmed().toLower();
        if (from != to.toLower() && ctx.db.queryInt("SELECT 1 FROM labels WHERE name_norm=?", to.toLower())) {
            // The new name exists here already: merge into it.
            ctx.db.run("INSERT OR IGNORE INTO msg_labels(msg_id, label_id) "
                       "SELECT ml.msg_id, (SELECT id FROM labels WHERE name_norm=?2) FROM msg_labels ml "
                       "JOIN labels l ON l.id = ml.label_id WHERE l.name_norm=?1",
                       from, to.toLower());
            ctx.db.run("DELETE FROM labels WHERE name_norm=? AND system=0", from);
        } else {
            ctx.db.run("UPDATE labels SET name=?, name_norm=?, color=? WHERE name_norm=? AND system=0", to,
                       to.toLower(), str(o, "color"), from);
        }
        return true;
    }
    if (o.kind == op::LabelDelete) {
        ctx.db.run("DELETE FROM labels WHERE name_norm=? AND system=0", str(o, "name").trimmed().toLower());
        return true;
    }
    if (o.kind == op::Accept) {
        mail::Contacts::touch(ctx, str(o, "addr"), QString(), ContactOrigin::Manual);
        mail::Organize::releaseHeld(ctx, str(o, "addr"));
        return true;
    }
    if (o.kind == op::Block) {
        const QString p = str(o, "pattern");
        ctx.db.run("INSERT OR IGNORE INTO blocklist(pattern_norm, pattern, created_at) VALUES(?, ?, ?)", p, p,
                   Hlc::physicalMs(o.hlc));
        return true;
    }
    if (o.kind == op::Unblock) {
        ctx.db.run("DELETE FROM blocklist WHERE pattern_norm=?", str(o, "pattern"));
        return true;
    }
    if (o.kind == op::ContactEdit) {
        const QString addr = str(o, "addr");
        mail::Contacts::touch(ctx, addr, QString(), ContactOrigin::Manual);
        ctx.db.run("UPDATE contacts SET name=? WHERE addr=?", str(o, "name").trimmed(), mime::normalizeAddr(addr));
        return true;
    }
    if (o.kind == op::ContactPolicy) {
        const qint64 id = mail::Contacts::touch(ctx, str(o, "addr"), QString(), ContactOrigin::Manual);
        const QJsonValue mode = o.payload.value(QLatin1String("mode"));
        if (!id)
            return false;
        if (mode.isDouble())
            ctx.db.run("INSERT OR REPLACE INTO contact_policy(contact_id, encryption_mode) VALUES(?, ?)", id,
                       mode.toInt());
        else
            ctx.db.run("DELETE FROM contact_policy WHERE contact_id=?", id);
        return true;
    }
    if (o.kind == op::ContactVerify) {
        const QString addr = str(o, "addr");
        mail::Contacts::touch(ctx, addr, QString(), ContactOrigin::Manual);
        ctx.db.run("UPDATE contacts SET fingerprint=?, verified=1 WHERE addr=?", str(o, "fpr"),
                   mime::normalizeAddr(addr));
        return true;
    }
    if (o.kind == op::ServerAck) {
        ctx.db.run("INSERT OR IGNORE INTO server_acks(remote_key, device) VALUES(?, ?)", str(o, "key"), o.device);
        return false;
    }
    if (o.kind == op::Setting) {
        const QString key = str(o, "key");
        if (!mail::Preferences::isSynced(key)) {
            qCWarning(lcSync) << "ignoring a synced setting that is not shared:" << key;
            return false;
        }
        mail::Preferences::apply(ctx, key, str(o, "value"));
        return true;
    }
    // key.learned: the key itself arrives with the message that taught it,
    // and Ingest learns it here the same way.
    return false;
}

} // namespace

OpApply::Result OpApply::apply(MailContext &ctx, const QList<Op> &ops)
{
    Result r;
    Transaction tx(ctx.db);
    Versions have = versions(ctx.db);
    QSet<QString> stalled;
    for (const Op &o : ops) {
        if (o.device.isEmpty() || o.kind.isEmpty() || stalled.contains(o.device))
            continue;
        const qint64 last = have.value(o.device, 0);
        if (o.seq <= last)
            continue; // already have it
        if (o.seq != last + 1) {
            stalled.insert(o.device);
            r.gap = true;
            continue;
        }
        const QString mkey = OpLog::mergeKey(o.kind, o.payload);
        ctx.db.run("INSERT INTO ops(device, seq, hlc, kind, payload, mkey) VALUES(?, ?, ?, ?, ?, ?)", o.device, o.seq,
                   o.hlc, o.kind, QString::fromUtf8(QJsonDocument(o.payload).toJson(QJsonDocument::Compact)),
                   nullIfEmpty(mkey));
        have.insert(o.device, o.seq);
        ++r.accepted;
        if (!mkey.isEmpty()) {
            const auto w = winner(ctx, mkey);
            if (!w || w->device != o.device || w->seq != o.seq)
                continue; // an op already here outranks it
        }
        if (applyOne(ctx, o, &r))
            r.changed = true;
    }
    tx.commit();
    return r;
}

void OpApply::replayFor(MailContext &ctx, const QString &messageId)
{
    QStringList keys{QStringLiteral("read:") + messageId, QStringLiteral("trash:") + messageId};
    // Every label key of this message: "label:<mid>:" up to, not including,
    // "label:<mid>;" (';' sorts right after ':').
    Statement st(ctx.db, "SELECT DISTINCT mkey FROM ops WHERE mkey >= ? AND mkey < ?");
    st.bindAll(QStringLiteral("label:%1:").arg(messageId), QStringLiteral("label:%1;").arg(messageId));
    while (st.step())
        keys.append(st.text(0));
    for (const QString &k : std::as_const(keys)) {
        if (const auto w = winner(ctx, k))
            applyOne(ctx, *w, nullptr);
    }
}

OpApply::Versions OpApply::versions(Database &db)
{
    Versions v;
    Statement st(db, "SELECT device, max(seq) FROM ops GROUP BY device");
    while (st.step())
        v.insert(st.text(0), st.int64(1));
    return v;
}

QList<Op> OpApply::missing(Database &db, const Versions &have, int limit)
{
    QList<Op> out;
    const Versions mine = versions(db);
    for (auto it = mine.cbegin(); it != mine.cend() && out.size() < limit; ++it) {
        const qint64 from = have.value(it.key(), 0);
        if (it.value() <= from)
            continue;
        Statement st(db, (QByteArray(kOpColumns) + "WHERE device=? AND seq>? ORDER BY seq LIMIT ?").constData());
        st.bindAll(it.key(), from, qint64(limit - out.size()));
        while (st.step())
            out.append(readOp(st));
    }
    return out;
}

bool OpApply::isTombstoned(Database &db, const QString &messageId)
{
    return db.queryInt("SELECT 1 FROM tombstones WHERE message_id=?", messageId).has_value();
}

} // namespace e3::sync
