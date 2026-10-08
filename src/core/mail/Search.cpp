// SPDX-License-Identifier: MPL-2.0
#include "Search.h"

#include "mime/Html.h"
#include "store/Config.h"
#include "store/Database.h"

namespace e3::mail {

namespace {

constexpr char kNotTrashed[] = "m.id NOT IN (SELECT msg_id FROM trashed)";
constexpr char kNotHeld[] = "m.id NOT IN (SELECT msg_id FROM held)";
constexpr char kArchived[] = "m.id IN (SELECT ml.msg_id FROM msg_labels ml JOIN labels l ON l.id = ml.label_id "
                             "WHERE l.system = 1 AND l.name_norm = 'archive')";
// Sent mail with our own address among its recipients: it was delivered to
// us too, so it belongs in the Inbox as well as in Sent (ADR 0007 amendment).
constexpr char kSentToSelf[] = "(m.direction = 1 AND m.state = 12 AND m.id IN (SELECT r.msg_id FROM recipients r "
                               "WHERE lower(r.addr) = (SELECT lower(trim(value)) FROM config WHERE key = 'addr')))";

QString ftsQuery(const QString &text)
{
    // Each word as a quoted prefix term, so user input can never be FTS syntax.
    QStringList terms;
    for (QString w : text.split(u' ', Qt::SkipEmptyParts)) {
        w.replace(u'"', QStringLiteral("\"\""));
        terms.append(u'"' + w + QStringLiteral("\"*"));
    }
    return terms.join(u' ');
}

QList<mime::Address> recipientsOf(MailContext &ctx, qint64 msgId, RecipientKind kind)
{
    QList<mime::Address> out;
    Statement st(ctx.db, "SELECT name, addr FROM recipients WHERE msg_id=? AND kind=? ORDER BY idx");
    st.bindAll(msgId, int(kind));
    while (st.step())
        out.append({st.text(0), st.text(1)});
    return out;
}

QList<LabelInfo> labelsOf(MailContext &ctx, qint64 msgId)
{
    QList<LabelInfo> out;
    Statement st(ctx.db, "SELECT l.id, l.name, l.color FROM msg_labels ml JOIN labels l ON l.id = ml.label_id "
                         "WHERE ml.msg_id=? AND l.system=0 ORDER BY l.name_norm");
    st.bind(1, msgId);
    while (st.step())
        out.append({st.int64(0), st.text(1), st.text(2), false, 0});
    return out;
}

constexpr char kSummaryColumns[] =
    "m.id, m.thread_id, m.direction, m.state, m.from_name, m.from_addr, m.subject, m.preview, m.date, "
    "m.unread, m.encrypted, m.signed_fpr IS NOT NULL AND m.signed_fpr != '', m.verified, m.importance, "
    "m.has_attachments";

void readSummary(MailContext &ctx, Statement &st, MessageSummary &s)
{
    s.id = st.int64(0);
    s.threadId = st.int64(1);
    s.direction = Direction(st.integer(2));
    s.state = MessageState(st.integer(3));
    s.from = {st.text(4), st.text(5)};
    s.subject = st.text(6);
    s.preview = st.text(7);
    s.date = QDateTime::fromMSecsSinceEpoch(st.int64(8));
    s.unread = st.integer(9);
    s.encrypted = st.integer(10);
    s.signedBySender = st.integer(11);
    s.verified = st.integer(12);
    s.importance = st.integer(13);
    s.hasAttachments = st.integer(14);
    s.to = recipientsOf(ctx, s.id, RecipientKind::To);
    s.labels = labelsOf(ctx, s.id);
}

} // namespace

QByteArray Search::tagClause(const QString &tag)
{
    const QByteArray notTrashed(kNotTrashed);
    if (tag == tag::Inbox)
        return "(m.direction = 0 OR " + QByteArray(kSentToSelf) + ") AND " + notTrashed + " AND " + kNotHeld
            + " AND NOT " + kArchived;
    if (tag == tag::Unverified)
        return "m.id IN (SELECT msg_id FROM held) AND " + notTrashed;
    if (tag == tag::Sent)
        return "m.direction = 1 AND m.state IN (11, 12, 13) AND " + notTrashed;
    if (tag == tag::Drafts)
        return "m.direction = 1 AND m.state = 10 AND " + notTrashed;
    if (tag == tag::Archive)
        return QByteArray(kArchived) + " AND " + notTrashed;
    if (tag == tag::Trash)
        return "m.id IN (SELECT msg_id FROM trashed)";
    // All mail: everything not thrown away and not waiting on a stranger.
    return notTrashed + " AND " + kNotHeld + " AND NOT (m.direction = 1 AND m.state = 10)";
}

QList<MessageSummary> Search::list(MailContext &ctx, const SearchQuery &q)
{
    QByteArray sql = QByteArray("SELECT ") + kSummaryColumns + " FROM messages m WHERE m.hidden = 0 AND ";
    if (q.labelId)
        sql += "m.id IN (SELECT msg_id FROM msg_labels WHERE label_id = ?1) AND " + QByteArray(kNotTrashed);
    else
        sql += tagClause(q.tag) + " AND ?1 = ?1";
    const QString fts = ftsQuery(q.text);
    if (!fts.isEmpty())
        sql += " AND m.id IN (SELECT rowid FROM msg_fts WHERE msg_fts MATCH ?2)";
    else
        sql += " AND ?2 IS NULL";
    sql += " ORDER BY m.date DESC LIMIT ?3";
    Statement st(ctx.db, sql.constData());
    st.bind(1, q.labelId);
    st.bind(2, nullIfEmpty(fts));
    st.bind(3, q.limit);
    QList<MessageSummary> out;
    while (st.step()) {
        MessageSummary s;
        readSummary(ctx, st, s);
        out.append(s);
    }
    return out;
}

std::optional<MessageDetail> Search::detail(MailContext &ctx, qint64 msgId)
{
    Statement st(ctx.db, (QByteArray("SELECT ") + kSummaryColumns
                          + ", m.message_id, m.in_reply_to, m.reply_to, m.body_text, m.body_html, m.raw_blob, "
                            "m.raw_expires_at, m.send_encryption FROM messages m WHERE m.id=?")
                             .constData());
    st.bind(1, msgId);
    if (!st.step())
        return std::nullopt;
    MessageDetail d;
    readSummary(ctx, st, d);
    d.messageId = st.text(15);
    d.inReplyTo = st.text(16);
    d.replyTo = st.text(17);
    d.bodyText = st.text(18);
    if (!st.isNull(19)) {
        const mime::SanitizedHtml clean = mime::sanitizeHtml(st.text(19));
        d.bodyHtml = clean.html;
        d.remoteBlocked = clean.remoteBlocked;
    }
    d.rawAvailable = !st.isNull(20);
    if (!st.isNull(21))
        d.rawExpiresAt = QDateTime::fromMSecsSinceEpoch(st.int64(21));
    if (!st.isNull(22))
        d.sendEncryption = SendEncryption(st.integer(22));
    d.cc = recipientsOf(ctx, msgId, RecipientKind::Cc);
    d.bcc = recipientsOf(ctx, msgId, RecipientKind::Bcc);

    Statement refs(ctx.db, "SELECT ref FROM msg_refs WHERE msg_id=? ORDER BY idx");
    refs.bind(1, msgId);
    while (refs.step())
        d.references.append(refs.text(0));

    Statement att(ctx.db,
                  "SELECT id, filename, mime_type, size, blob, content_id FROM attachments WHERE msg_id=? ORDER BY idx");
    att.bind(1, msgId);
    while (att.step())
        d.attachments.append({att.int64(0), att.text(1), att.text(2), att.int64(3), att.text(4), att.text(5)});

    Statement und(ctx.db, "SELECT addr FROM undelivered WHERE msg_id=? ORDER BY addr");
    und.bind(1, msgId);
    while (und.step())
        d.undelivered.append(und.text(0));

    Statement tr(ctx.db, "SELECT reason, purge_at FROM trashed WHERE msg_id=?");
    tr.bind(1, msgId);
    if (tr.step()) {
        d.trashed = true;
        d.trashReason = TrashReason(tr.integer(0));
        d.purgeAt = QDateTime::fromMSecsSinceEpoch(tr.int64(1));
    }
    if (const auto heldAt = ctx.db.queryInt("SELECT held_at FROM held WHERE msg_id=?", msgId)) {
        d.held = true;
        d.heldAt = QDateTime::fromMSecsSinceEpoch(*heldAt);
    }
    d.archived = ctx.db
                     .queryInt("SELECT 1 FROM msg_labels ml JOIN labels l ON l.id = ml.label_id "
                               "WHERE ml.msg_id=? AND l.system=1 AND l.name_norm='archive'",
                               msgId)
                     .has_value();
    return d;
}

QList<qint64> Search::thread(MailContext &ctx, qint64 msgId)
{
    const bool trashed = ctx.db.queryInt("SELECT 1 FROM trashed WHERE msg_id=?", msgId).has_value();
    Statement st(ctx.db, "SELECT m.id FROM messages m WHERE m.thread_id = (SELECT thread_id FROM messages WHERE id=?1) "
                         "AND m.hidden = 0 AND (m.id = ?1 OR ?2 OR m.id NOT IN (SELECT msg_id FROM trashed)) "
                         "AND NOT (m.direction = 1 AND m.state = 10 AND m.id != ?1) "
                         "ORDER BY m.date ASC, m.id ASC");
    st.bind(1, msgId);
    st.bind(2, trashed);
    QList<qint64> out;
    while (st.step())
        out.append(st.int64(0));
    if (out.isEmpty())
        out.append(msgId);
    return out;
}

int Search::count(MailContext &ctx, const QString &tag, bool unreadOnly)
{
    QByteArray sql = "SELECT count(*) FROM messages m WHERE m.hidden = 0 AND " + tagClause(tag);
    if (unreadOnly)
        sql += " AND m.unread = 1";
    return int(ctx.db.queryInt(sql.constData()).value_or(0));
}

void Search::index(MailContext &ctx, qint64 msgId)
{
    Statement st(ctx.db, "SELECT subject, body_text, from_name || ' ' || from_addr || ' ' || "
                         "coalesce((SELECT group_concat(name || ' ' || addr, ' ') FROM recipients WHERE msg_id=?1), '') "
                         "FROM messages WHERE id=?1");
    st.bind(1, msgId);
    if (!st.step())
        return;
    ctx.db.run("DELETE FROM msg_fts WHERE rowid=?", msgId);
    ctx.db.run("INSERT INTO msg_fts(rowid, subject, body, people) VALUES(?, ?, ?, ?)", msgId, st.text(0), st.text(1),
               st.text(2));
}

} // namespace e3::mail
