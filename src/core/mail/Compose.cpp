// SPDX-License-Identifier: MPL-2.0
#include "Compose.h"

#include "Contacts.h"
#include "Ingest.h"
#include "Keyring.h"
#include "Organize.h"
#include "Policy.h"
#include "Search.h"
#include "Threading.h"
#include "crypto/Autocrypt.h"
#include "crypto/Pgp.h"
#include "mime/Builder.h"
#include "mime/Html.h"
#include "store/BlobStore.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"
#include "util/Log.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QLocale>
#include <QTimeZone>

namespace e3::mail {

namespace {

constexpr int kMaxAttempts = 10;

QString ensurePrefix(const QString &subject, const QString &prefix)
{
    const QString t = subject.trimmed();
    if (t.startsWith(prefix, Qt::CaseInsensitive))
        return t;
    return prefix + u' ' + t;
}

QStringList allAddrs(const Draft &d)
{
    QStringList out;
    for (const auto &list : {d.to, d.cc, d.bcc}) {
        for (const mime::Address &a : list) {
            const QString n = mime::normalizeAddr(a.addr);
            if (!n.isEmpty() && !out.contains(n))
                out.append(n);
        }
    }
    return out;
}

void writeBody(MailContext &ctx, qint64 id, const Draft &d)
{
    ctx.db.run("DELETE FROM recipients WHERE msg_id=?", id);
    int kindIdx = 0;
    for (const auto &list : {d.to, d.cc, d.bcc}) {
        int idx = 0;
        for (const mime::Address &a : list)
            ctx.db.run("INSERT OR IGNORE INTO recipients(msg_id, kind, idx, addr, name) VALUES(?, ?, ?, ?, ?)", id,
                       kindIdx, idx++, a.addr.trimmed(), a.name.trimmed());
        ++kindIdx;
    }
    ctx.db.run("DELETE FROM attachments WHERE msg_id=?", id);
    for (qsizetype i = 0; i < d.attachments.size(); ++i) {
        const OutgoingAttachmentRef &a = d.attachments[i];
        ctx.db.run("INSERT INTO attachments(msg_id, idx, filename, mime_type, size, blob) VALUES(?, ?, ?, ?, ?, ?)",
                   id, int(i), a.filename, a.mimeType, qint64(a.data.size()), ctx.blobs.put(a.data));
    }
}

struct Parent
{
    QString messageId;
    QStringList references;
};

std::optional<Parent> parentOf(MailContext &ctx, qint64 parentId)
{
    if (!parentId)
        return std::nullopt;
    const auto d = Search::detail(ctx, parentId);
    if (!d)
        return std::nullopt;
    Parent p{d->messageId, d->references};
    if (p.references.isEmpty() && !d->inReplyTo.isEmpty())
        p.references.append(d->inReplyTo);
    p.references.append(d->messageId);
    // Keep References bounded the way other clients do: first plus the tail.
    while (p.references.size() > 20)
        p.references.removeAt(1);
    return p;
}

} // namespace

qint64 Compose::saveDraft(MailContext &ctx, const Draft &d)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto parent = parentOf(ctx, d.inReplyToId);
    Transaction tx(ctx.db);
    qint64 id = d.id;
    const bool exists = id && ctx.db.queryInt("SELECT 1 FROM messages WHERE id=? AND state=?", id,
                                              int(MessageState::Draft)).has_value();
    const auto html = nullIfEmpty(d.html);
    const QString sendEnc = d.encryption == SendEncryption::Auto ? QString() : QString::number(int(d.encryption));
    if (exists) {
        ctx.db.run("UPDATE messages SET subject=?, date=?, body_text=?, body_html=?, preview=?, importance=?, "
                   "has_attachments=?, send_encryption=? WHERE id=?",
                   d.subject, now, d.text, html, d.text.simplified().left(200), d.importance,
                   !d.attachments.isEmpty(), sendEnc.isEmpty() ? QVariant() : QVariant(sendEnc.toInt()), id);
    } else {
        const QString addr = ctx.config.get(cfg::Addr);
        ctx.db.run("INSERT INTO messages(message_id, direction, state, subject, from_addr, from_name, date, "
                   "received_at, in_reply_to, body_text, body_html, preview, importance, has_attachments, "
                   "send_encryption) VALUES(?, 1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                   mime::generateMessageId(addr), int(MessageState::Draft), d.subject, addr,
                   ctx.config.get(cfg::DisplayName), now, now, parent ? parent->messageId : QString(), d.text, html,
                   d.text.simplified().left(200), d.importance, !d.attachments.isEmpty(),
                   sendEnc.isEmpty() ? QVariant() : QVariant(sendEnc.toInt()));
        id = ctx.db.lastInsertId();
        if (parent) {
            for (qsizetype i = 0; i < parent->references.size(); ++i)
                ctx.db.run("INSERT INTO msg_refs(msg_id, idx, ref) VALUES(?, ?, ?)", id, int(i), parent->references[i]);
        }
        const QString mid = Organize::messageIdOf(ctx, id);
        Threading::assign(ctx, id, mid, parent ? parent->messageId : QString(),
                          parent ? parent->references : QStringList(), now);
    }
    writeBody(ctx, id, d);
    tx.commit();
    return id;
}

Draft Compose::loadDraft(MailContext &ctx, qint64 msgId)
{
    Draft d;
    const auto m = Search::detail(ctx, msgId);
    if (!m)
        return d;
    d.id = msgId;
    d.to = m->to;
    d.cc = m->cc;
    d.bcc = m->bcc;
    d.subject = m->subject;
    d.text = m->bodyText;
    d.html = ctx.db.queryText("SELECT body_html FROM messages WHERE id=?", msgId).value_or(QString());
    d.importance = m->importance;
    d.encryption = m->sendEncryption;
    if (!m->inReplyTo.isEmpty())
        d.inReplyToId = ctx.db.queryInt("SELECT id FROM messages WHERE message_id=?", m->inReplyTo).value_or(0);
    for (const AttachmentInfo &a : m->attachments)
        d.attachments.append({a.filename, a.mimeType, ctx.blobs.get(a.blob)});
    return d;
}

void Compose::deleteDraft(MailContext &ctx, qint64 msgId)
{
    Transaction tx(ctx.db);
    if (ctx.db.queryInt("SELECT 1 FROM messages WHERE id=? AND state=?", msgId, int(MessageState::Draft)))
        Organize::purge(ctx, msgId);
    tx.commit();
}

qint64 Compose::queue(MailContext &ctx, const Draft &draft)
{
    const QStringList rcpts = allAddrs(draft);
    if (rcpts.isEmpty())
        throw ComposeError(QCoreApplication::translate("Policy", "Add a recipient."));
    const Policy::Readiness ready = Policy::evaluate(ctx, rcpts, draft.encryption);
    if (!ready.canSend)
        throw ComposeError(ready.refusal);

    const QString selfAddr = ctx.config.get(cfg::Addr);
    const QString selfFpr = Keyring::ensureSelfKey(ctx, selfAddr);
    const qint64 id = saveDraft(ctx, draft);
    const auto parent = parentOf(ctx, draft.inReplyToId);

    mime::OutgoingMail mail;
    mail.from = {ctx.config.get(cfg::DisplayName), selfAddr};
    mail.to = draft.to;
    mail.cc = draft.cc;
    mail.subject = draft.subject;
    // Whole seconds, as the Date header carries, so the stored date and the
    // one every recipient sees sort the same way.
    mail.date = QDateTime::fromSecsSinceEpoch(QDateTime::currentSecsSinceEpoch(), QTimeZone::UTC);
    mail.messageId = Organize::messageIdOf(ctx, id);
    if (parent) {
        mail.inReplyTo = parent->messageId;
        mail.references = parent->references;
    }
    mail.text = draft.text;
    mail.html = draft.html;
    mail.importance = draft.importance;
    for (const OutgoingAttachmentRef &a : draft.attachments)
        mail.attachments.append({a.filename, a.mimeType.toLatin1(), a.data});
    mail.extraHeaders.append(
        {"Autocrypt", crypto::Autocrypt::format(selfAddr, ctx.pgp.exportAutocrypt(selfFpr, selfAddr), true)});

    QByteArray raw;
    if (ready.willEncrypt) {
        // Gossip every recipient's key to the others, so a reply-all can
        // encrypt without each of them having met.
        const QStringList visible = allAddrs(Draft{0, draft.to, draft.cc, {}, {}, {}, {}, {}, 0, 0, {}});
        if (visible.size() > 1) {
            for (const QString &addr : visible) {
                const auto c = Contacts::get(ctx, addr);
                if (c && !c->fingerprint.isEmpty() && Keyring::ensure(ctx, c->fingerprint))
                    mail.extraHeaders.append(
                        {"Autocrypt-Gossip",
                         crypto::Autocrypt::format(addr, ctx.pgp.exportAutocrypt(c->fingerprint, addr), false)});
            }
        }
        const QByteArray inner = mime::Builder::contentEntity(mail, true);
        const QByteArray armored = ctx.pgp.encrypt(inner, ready.recipientFprs + QStringList{selfFpr}, selfFpr);
        const QString hidden = QStringLiteral("...");
        const QByteArray boundary = mime::Builder::newBoundary();
        raw = mime::Builder::headerBlock(mail, &hidden);
        raw += "Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\"; boundary=\"" + boundary
            + "\"\r\n\r\nThis is an OpenPGP/MIME encrypted message (RFC 4880 and 3156).\r\n--" + boundary
            + "\r\nContent-Type: application/pgp-encrypted\r\nContent-Description: PGP/MIME version identification"
              "\r\n\r\nVersion: 1\r\n\r\n--"
            + boundary
            + "\r\nContent-Type: application/octet-stream; name=\"encrypted.asc\"\r\n"
              "Content-Description: OpenPGP encrypted message\r\n"
              "Content-Disposition: inline; filename=\"encrypted.asc\"\r\n\r\n"
            + armored + "\r\n--" + boundary + "--\r\n";
    } else {
        raw = mime::Builder::build(mail);
    }

    const qint64 now = mail.date.toMSecsSinceEpoch();
    const int rawDays = ctx.config.getInt(cfg::RawMimeDays);
    const QString rawBlob = rawDays != 0 ? ctx.blobs.put(raw) : QString();
    std::optional<qint64> rawExpires;
    if (rawDays > 0)
        rawExpires = now + qint64(rawDays) * 24 * 3600 * 1000;

    Transaction tx(ctx.db);
    ctx.db.run("UPDATE messages SET state=?, date=?, encrypted=?, signed_fpr=?, raw_blob=?, raw_expires_at=?, "
               "preview=? WHERE id=?",
               int(MessageState::Pending), now, ready.willEncrypt, ready.willEncrypt ? selfFpr : QString(),
               nullIfEmpty(rawBlob), rawExpires, Ingest::makePreview(draft.text), id);
    ctx.db.run("INSERT INTO outbox(msg_id, raw, mail_from, rcpts) VALUES(?, ?, ?, ?)", id, raw, selfAddr,
               rcpts.join(u'\n'));
    // Writing to someone makes them known, and releases any mail of theirs
    // waiting in Unverified.
    for (const auto &list : {draft.to, draft.cc, draft.bcc}) {
        for (const mime::Address &a : list) {
            Contacts::touch(ctx, a.addr, a.name, ContactOrigin::Outgoing);
            Organize::releaseHeld(ctx, a.addr);
        }
    }
    Search::index(ctx, id);
    ctx.ops.record(op::MessageAdded, {{QStringLiteral("mid"), mail.messageId}, {QStringLiteral("raw"), rawBlob}});
    tx.commit();
    qCInfo(lcMail) << "queued message" << id << (ready.willEncrypt ? "encrypted" : "cleartext") << "to" << rcpts;
    return id;
}

Draft Compose::reply(MailContext &ctx, qint64 msgId, bool all)
{
    Draft d;
    const auto m = Search::detail(ctx, msgId);
    if (!m)
        return d;
    const QString self = mime::normalizeAddr(ctx.config.get(cfg::Addr));
    d.inReplyToId = msgId;
    if (m->direction == Direction::Outgoing) {
        d.to = m->to; // replying to our own message continues to its recipients
    } else if (!m->replyTo.isEmpty()) {
        d.to = {{QString(), m->replyTo}};
    } else {
        d.to = {m->from};
    }
    if (all) {
        for (const mime::Address &a : m->to + m->cc) {
            const QString n = mime::normalizeAddr(a.addr);
            bool dup = n == self;
            for (const mime::Address &x : d.to + d.cc)
                dup = dup || mime::normalizeAddr(x.addr) == n;
            if (!dup)
                d.cc.append(a);
        }
    }
    d.subject = ensurePrefix(m->subject, QStringLiteral("Re:"));
    QString quoted;
    for (const QString &line : m->bodyText.split(u'\n'))
        quoted += (line.startsWith(u'>') ? QStringLiteral(">") : QStringLiteral("> ")) + line + u'\n';
    const QString who = m->from.name.isEmpty() ? m->from.addr : m->from.name;
    // In the writer's language, as other clients do. "Re:" stays as it is:
    // a translated prefix breaks threading in the recipient's client.
    const QDateTime when = m->date.toLocalTime();
    const QString attribution = QCoreApplication::translate("Compose", "On %1 at %2, %3 wrote:")
                                    .arg(QLocale().toString(when.date(), QLocale::LongFormat),
                                         QLocale().toString(when.time(), QLocale::ShortFormat), who);
    d.text = QStringLiteral("\n\n%1\n%2").arg(attribution, quoted);
    // HTML mail is quoted as HTML, so its formatting survives. It is what was
    // stored, already sanitized; the composer re-emits it through its own
    // whitelist before anything is sent.
    if (!m->bodyHtml.isEmpty())
        d.html = QStringLiteral("<p><br></p><p><br></p><p>") + mime::escapeHtml(attribution)
            + QStringLiteral("</p><blockquote>") + m->bodyHtml + QStringLiteral("</blockquote>");
    // A reply to encrypted mail asks for encryption.
    d.encryption = m->encrypted ? SendEncryption::Required : SendEncryption::Auto;
    return d;
}

Draft Compose::forward(MailContext &ctx, qint64 msgId)
{
    Draft d;
    const auto m = Search::detail(ctx, msgId);
    if (!m)
        return d;
    d.subject = ensurePrefix(m->subject, QStringLiteral("Fwd:"));
    QStringList toList;
    for (const mime::Address &a : m->to)
        toList.append(a.display());
    d.text = QCoreApplication::translate("Compose", "\n\n---------- Forwarded message ----------\nFrom: %1\nDate: %2\n"
                                                    "Subject: %3\nTo: %4\n\n")
                 .arg(m->from.display(), QLocale().toString(m->date.toLocalTime(), QLocale::LongFormat), m->subject,
                      toList.join(QStringLiteral(", ")))
        + m->bodyText;
    if (!m->bodyHtml.isEmpty()) {
        const QString header = d.text.left(d.text.size() - m->bodyText.size()).trimmed();
        d.html = QStringLiteral("<p><br></p><p><br></p>") + mime::textToHtml(header) + m->bodyHtml;
    }
    for (const AttachmentInfo &a : m->attachments)
        d.attachments.append({a.filename, a.mimeType, ctx.blobs.get(a.blob)});
    return d;
}

QList<Compose::Outbox> Compose::dueOutbox(MailContext &ctx)
{
    QList<Outbox> out;
    Statement st(ctx.db, "SELECT id, msg_id, raw, mail_from, rcpts, attempts FROM outbox WHERE next_attempt <= ? "
                         "ORDER BY id");
    st.bind(1, QDateTime::currentMSecsSinceEpoch());
    while (st.step())
        out.append({st.int64(0), st.int64(1), st.blob(2), st.text(3), st.text(4).split(u'\n', Qt::SkipEmptyParts),
                    st.integer(5)});
    return out;
}

void Compose::markSent(MailContext &ctx, const Outbox &o, const QStringList &refused)
{
    Transaction tx(ctx.db);
    ctx.db.run("DELETE FROM outbox WHERE id=?", o.id);
    ctx.db.run("UPDATE messages SET state=? WHERE id=?", int(MessageState::Sent), o.msgId);
    for (const QString &r : refused)
        ctx.db.run("INSERT OR IGNORE INTO undelivered(msg_id, addr) VALUES(?, ?)", o.msgId, r);
    tx.commit();
}

void Compose::markFailed(MailContext &ctx, const Outbox &o, const QString &error)
{
    const int attempts = o.attempts + 1;
    if (attempts >= kMaxAttempts) {
        Transaction tx(ctx.db);
        ctx.db.run("DELETE FROM outbox WHERE id=?", o.id);
        ctx.db.run("UPDATE messages SET state=? WHERE id=?", int(MessageState::Failed), o.msgId);
        tx.commit();
        qCWarning(lcMail) << "giving up on message" << o.msgId << error;
        return;
    }
    // Exponential backoff: 30 s, 1 min, 2 min ... capped at an hour.
    const qint64 delay = qMin<qint64>(3600, 30LL << qMin(attempts - 1, 7)) * 1000;
    ctx.db.run("UPDATE outbox SET attempts=?, next_attempt=?, last_error=? WHERE id=?", attempts,
               QDateTime::currentMSecsSinceEpoch() + delay, error, o.id);
}

} // namespace e3::mail
