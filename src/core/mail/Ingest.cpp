// SPDX-License-Identifier: MPL-2.0
#include "Ingest.h"

#include "Contacts.h"
#include "Keyring.h"
#include "Organize.h"
#include "Search.h"
#include "SecureJoin.h"
#include "Threading.h"
#include "crypto/Autocrypt.h"
#include "crypto/Pgp.h"
#include "mime/Encoding.h"
#include "mime/Html.h"
#include "store/BlobStore.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"
#include "sync/MailboxFallback.h"
#include "sync/OpApply.h"
#include "util/Log.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonObject>

namespace e3::mail {

namespace {

constexpr qint64 kDayMs = 24LL * 3600 * 1000;

// Headers an encrypted part may protect (RFC 9788); inner copies win.
const QList<QByteArray> kProtected = {"From", "To", "Cc", "Reply-To", "Subject", "Date", "Message-ID",
                                      "In-Reply-To", "References", "Importance", "X-Priority", "Priority",
                                      // SecureJoin (ADR 0021): its steps and tokens travel inside.
                                      "Secure-Join", "Secure-Join-Auth", "Secure-Join-Fingerprint",
                                      "Secure-Join-Invitenumber"};

bool isEncryptedPart(const mime::Part &p)
{
    return p.mimeType == "multipart/encrypted" && p.children.size() >= 2
        && p.typeParams.value("protocol").toLower() == QLatin1String("application/pgp-encrypted");
}

// Signed without encryption, or signed inside it (RFC 3156).
bool isSignedPart(const mime::Part &p)
{
    return p.mimeType == "multipart/signed" && p.children.size() == 2
        && p.typeParams.value("protocol").toLower() == QLatin1String("application/pgp-signature");
}

// Good signers of a multipart/signed. The signature covers the first part
// exactly as sent, headers included, with CRLF line endings.
QStringList signersOf(MailContext &ctx, const mime::Part &signedPart)
{
    QByteArray data = signedPart.children[0].raw;
    data.replace("\r\n", "\n");
    data.replace("\n", "\r\n");
    try {
        return ctx.pgp.verifyDetached(data, signedPart.children[1].body);
    } catch (const crypto::PgpError &e) {
        qCWarning(lcMail) << "cannot read a signature:" << e.what();
        return {};
    }
}

// The part `walk` shows as the text, or null.
const mime::Part *textPart(const mime::Part &p)
{
    if (p.isMultipart()) {
        if (p.mimeType == "multipart/alternative") {
            const mime::Part *text = nullptr;
            for (const mime::Part &c : p.children) {
                if (c.mimeType == "text/plain")
                    text = &c;
            }
            if (text)
                return text;
        }
        for (const mime::Part &c : p.children) {
            if (const mime::Part *t = textPart(c))
                return t;
        }
        return nullptr;
    }
    return p.disposition != "attachment" && p.mimeType == "text/plain" ? &p : nullptr;
}

// Inline PGP in a text body: the first armoured message or cleartext-signed
// block, as [start, end) in the part's bytes.
std::optional<std::pair<qsizetype, qsizetype>> armouredBlock(const QByteArray &body)
{
    static const QByteArray kMessage = "-----BEGIN PGP MESSAGE-----";
    static const QByteArray kSigned = "-----BEGIN PGP SIGNED MESSAGE-----";
    for (qsizetype pos = 0; pos < body.size();) {
        const qsizetype nl = body.indexOf('\n', pos);
        const qsizetype lineEnd = nl < 0 ? body.size() : nl;
        const QByteArray line = body.mid(pos, lineEnd - pos).trimmed();
        if (line == kMessage || line == kSigned) {
            const QByteArray end = line == kMessage ? "-----END PGP MESSAGE-----" : "-----END PGP SIGNATURE-----";
            const qsizetype e = body.indexOf(end, lineEnd);
            if (e < 0)
                return std::nullopt;
            const qsizetype eol = body.indexOf('\n', e);
            return std::make_pair(pos, eol < 0 ? body.size() : eol + 1);
        }
        pos = lineEnd + 1;
    }
    return std::nullopt;
}

void walk(const mime::Part &p, Ingest::Content &out, bool inAlternative)
{
    if (p.isMultipart()) {
        if (p.mimeType == "multipart/alternative") {
            // Prefer the last representation of each kind, per RFC 2046.
            const mime::Part *text = nullptr;
            const mime::Part *html = nullptr;
            for (const mime::Part &c : p.children) {
                if (c.mimeType == "text/plain")
                    text = &c;
                else if (c.mimeType == "text/html")
                    html = &c;
                else if (c.isMultipart())
                    walk(c, out, true);
            }
            if (text && out.text.isEmpty())
                out.text = text->text();
            if (html && out.html.isEmpty())
                out.html = html->text();
            return;
        }
        for (const mime::Part &c : p.children)
            walk(c, out, inAlternative);
        return;
    }
    const bool attachment = p.disposition == "attachment"
        || (!p.filename.isEmpty() && !p.mimeType.startsWith("text/"));
    // Machine-readable and signature parts are not something to show.
    if (p.mimeType == "application/pgp-signature" || p.mimeType == "application/pgp-encrypted")
        return;
    if (!attachment && p.mimeType == "text/plain" && out.text.isEmpty()) {
        out.text = p.text();
        return;
    }
    if (!attachment && p.mimeType == "text/html" && out.html.isEmpty()) {
        out.html = p.text();
        return;
    }
    if (!attachment && p.mimeType.startsWith("text/") && !out.text.isEmpty()) {
        out.text += QStringLiteral("\n\n") + p.text(); // a second inline text part
        return;
    }
    QString name = p.filename;
    if (name.isEmpty())
        name = p.mimeType == "message/rfc822" ? QStringLiteral("message.eml") : QStringLiteral("attachment");
    out.attachments.append({name, QString::fromLatin1(p.mimeType), p.contentId, p.body});
}

} // namespace

Ingest::Content Ingest::extract(const mime::Part &root)
{
    Content c;
    walk(root, c, false);
    if (c.text.isEmpty() && !c.html.isEmpty())
        c.text = mime::htmlToText(c.html);
    c.text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    return c;
}

int Ingest::importanceOf(const mime::HeaderList &h)
{
    const QByteArray imp = h.raw("Importance").trimmed().toLower();
    if (imp == "high")
        return 1;
    if (imp == "low")
        return -1;
    if (imp == "normal")
        return 0;
    const QByteArray xp = h.raw("X-Priority").trimmed();
    if (!xp.isEmpty() && std::isdigit(static_cast<unsigned char>(xp[0]))) {
        const int v = xp.left(1).toInt();
        return v <= 2 ? 1 : (v >= 4 ? -1 : 0);
    }
    const QByteArray pr = h.raw("Priority").trimmed().toLower();
    if (pr == "urgent")
        return 1;
    if (pr == "non-urgent")
        return -1;
    return 0;
}

QString Ingest::makePreview(const QString &text)
{
    QStringList kept;
    const QStringList lines = text.split(u'\n');
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString t = lines[i].trimmed();
        if (t.startsWith(u'>') || t == QLatin1String("--"))
            continue;
        // The attribution line of a quoted reply, in any language: it ends
        // with a colon and the quote follows.
        if (t.endsWith(u':') || t.endsWith(u'\uFF1A')) {
            qsizetype j = i + 1;
            while (j < lines.size() && lines[j].trimmed().isEmpty())
                ++j;
            if (j < lines.size() && lines[j].trimmed().startsWith(u'>'))
                break;
        }
        if (!t.isEmpty())
            kept.append(t);
        if (kept.join(u' ').size() > 240)
            break;
    }
    return kept.join(u' ').left(200);
}

Ingest::Result Ingest::process(MailContext &ctx, const QByteArray &raw)
{
    Result result;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const mime::Part outer = mime::parse(raw);
    mime::HeaderList headers = outer.headers;

    // Hidden machine mail (device sync, SecureJoin) is recorded but never
    // shown. SecureJoin is known only once decrypted.
    const bool deviceSync = headers.has("X-E3-Sync");

    // Decrypt, then let protected inner headers win over the outer ones.
    mime::Part content = outer;
    bool encrypted = false;
    QStringList validSigners;
    QString decryptError;
    if (isEncryptedPart(outer)) {
        try {
            const crypto::Pgp::Decrypted d = ctx.pgp.decrypt(outer.children[1].body);
            content = mime::parse(d.data);
            encrypted = d.encrypted;
            validSigners = d.validSigners;
            for (const QByteArray &name : kProtected) {
                const QList<QByteArray> inner = content.headers.all(name);
                if (!inner.isEmpty()) {
                    headers.remove(name);
                    for (const QByteArray &v : inner)
                        headers.append(name, v);
                }
            }
            for (const QByteArray &v : content.headers.all("Autocrypt"))
                headers.append("Autocrypt", v);
        } catch (const crypto::PgpError &e) {
            decryptError = QString::fromUtf8(e.what());
            qCWarning(lcMail) << "cannot decrypt:" << decryptError;
        }
    }

    const bool handshake = SecureJoin::isHandshake(headers);
    const bool hidden = deviceSync || handshake;
    result.hidden = hidden;

    std::optional<mime::Part> signedPart;
    if (decryptError.isEmpty() && isSignedPart(content)) {
        signedPart = content;
        content = signedPart->children[0];
        validSigners += signersOf(ctx, *signedPart);
    }

    QString messageId = mime::parseMessageIds(headers.raw("Message-ID")).value(0);
    if (messageId.isEmpty()) {
        // Stable across fetches, so a refetch is still a duplicate.
        messageId = QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex().left(32))
            + QStringLiteral("@e3mail.invalid");
    }
    if (const auto existing = ctx.db.queryInt("SELECT id FROM messages WHERE message_id=?", messageId)) {
        result.msgId = *existing;
        result.duplicate = true;
        return result;
    }
    // Purged here or on another device: it stays gone.
    if (sync::OpApply::isTombstoned(ctx.db, messageId)) {
        result.duplicate = true;
        result.purged = true;
        return result;
    }

    // Device sync by mail: its ops, applied before the message is stored
    // (hidden), outside the transaction that stores it.
    if (deviceSync && isEncryptedPart(outer) && decryptError.isEmpty())
        sync::MailboxFallback::receive(ctx, extract(content).text, validSigners);

    const QList<mime::Address> fromList = mime::parseAddressList(headers.raw("From"));
    const mime::Address from = fromList.value(0);
    const QList<mime::Address> to = mime::parseAddressList(headers.raw("To"));
    const QList<mime::Address> cc = mime::parseAddressList(headers.raw("Cc"));
    const QList<mime::Address> replyTo = mime::parseAddressList(headers.raw("Reply-To"));
    QString subject = headers.text("Subject");
    QDateTime date = mime::parseDate(headers.raw("Date"));
    if (!date.isValid() || date.toMSecsSinceEpoch() > now + kDayMs)
        date = QDateTime::fromMSecsSinceEpoch(now);
    const QString inReplyTo = mime::parseMessageIds(headers.raw("In-Reply-To")).value(0);
    const QStringList references = mime::parseMessageIds(headers.raw("References"));
    const int importance = importanceOf(headers);

    Content body = extract(content);
    if (!decryptError.isEmpty()) {
        body = Content();
        body.text = QCoreApplication::translate("Ingest", "This message is encrypted and could not be decrypted on this device.\n\n%1")
                        .arg(decryptError);
        encrypted = true;
    }

    // Inline PGP, in mail without PGP/MIME: the armour in the text is
    // replaced by what it holds. Tried again once the keys this message
    // carries are learned, as for PGP/MIME (trap 4).
    const mime::Part *inlinePart = !encrypted && !signedPart && decryptError.isEmpty() ? textPart(content) : nullptr;
    const auto inlineRange = inlinePart ? armouredBlock(inlinePart->body) : std::nullopt;
    bool inlineDone = false;
    const auto tryInline = [&] {
        if (!inlineRange || inlineDone)
            return;
        const auto [start, end] = *inlineRange;
        try {
            const crypto::Pgp::Decrypted d = ctx.pgp.decrypt(inlinePart->body.mid(start, end - start));
            const QByteArray cs = inlinePart->charset();
            QString text = mime::decodeCharset(inlinePart->body.left(start), cs) + mime::decodeCharset(d.data, cs)
                + mime::decodeCharset(inlinePart->body.mid(end), cs);
            text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            body.text = text;
            body.html.clear(); // an HTML alternative would still show the armour
            encrypted = d.encrypted;
            validSigners = d.validSigners;
            inlineDone = true;
        } catch (const crypto::PgpError &e) {
            qCWarning(lcMail) << "cannot read inline PGP:" << e.what();
        }
    };
    tryInline();

    const QString selfAddr = mime::normalizeAddr(ctx.config.get(cfg::Addr));
    const bool fromSelf = mime::normalizeAddr(from.addr) == selfAddr;

    // Blobs first: they are content-addressed, so a crash before commit only
    // leaves an unreferenced file for housekeeping to collect.
    QString rawBlob;
    std::optional<qint64> rawExpires;
    const int rawDays = ctx.config.getInt(cfg::RawMimeDays);
    if (rawDays != 0) {
        rawBlob = ctx.blobs.put(raw);
        if (rawDays > 0)
            rawExpires = now + rawDays * kDayMs;
    }
    QList<QString> attachmentBlobs;
    for (const Content::Attachment &a : std::as_const(body.attachments))
        attachmentBlobs.append(ctx.blobs.put(a.data));

    Transaction tx(ctx.db);

    // Contacts: the sender wrote to us; everyone else on the message was seen.
    if (!fromSelf)
        Contacts::touch(ctx, from.addr, from.name, ContactOrigin::IncomingFrom);
    for (const mime::Address &a : to + cc) {
        if (mime::normalizeAddr(a.addr) != selfAddr)
            Contacts::touch(ctx, a.addr, a.name, ContactOrigin::IncomingCopy);
    }

    // Keys. The Autocrypt header earns an unverified key; a valid signature
    // by the sender's known key is what "signed" means here.
    if (!fromSelf) {
        if (const auto ac = crypto::Autocrypt::select(headers.all("Autocrypt"), from.addr)) {
            try {
                const QStringList fprs = Keyring::store(ctx, ac->keydata);
                if (!fprs.isEmpty())
                    Contacts::adoptKey(ctx, from.addr, fprs.first(), ac->preferEncrypt, date, false);
            } catch (const crypto::PgpError &e) {
                qCWarning(lcMail) << "ignoring unusable Autocrypt key from" << from.addr << e.what();
            }
        }
        if (encrypted) {
            for (const QByteArray &g : content.headers.all("Autocrypt-Gossip")) {
                const auto gossip = crypto::Autocrypt::parse(g);
                if (!gossip || gossip->addr == selfAddr)
                    continue;
                try {
                    const QStringList fprs = Keyring::store(ctx, gossip->keydata);
                    if (!fprs.isEmpty())
                        Contacts::adoptKey(ctx, gossip->addr, fprs.first(), false, date, true);
                } catch (const crypto::PgpError &) {
                }
            }
        }
    }
    // A first encrypted message carries the key that signed it; verify again
    // now that the keyring has it.
    if (isEncryptedPart(outer) && decryptError.isEmpty() && validSigners.isEmpty()) {
        try {
            validSigners = ctx.pgp.decrypt(outer.children[1].body).validSigners;
        } catch (const crypto::PgpError &) {
        }
    }
    if (signedPart && validSigners.isEmpty())
        validSigners = signersOf(ctx, *signedPart);
    if (inlineRange && validSigners.isEmpty()) {
        inlineDone = false;
        tryInline();
    }
    QString signedFpr;
    bool verified = false;
    if (const auto sender = Contacts::get(ctx, from.addr)) {
        if (!sender->fingerprint.isEmpty() && validSigners.contains(sender->fingerprint)) {
            signedFpr = sender->fingerprint;
            verified = sender->verified;
        }
    }
    if (fromSelf && validSigners.contains(Keyring::selfFingerprint(ctx)))
        signedFpr = Keyring::selfFingerprint(ctx);

    // SecureJoin: the next step, now that the keys this message carries
    // are known.
    if (handshake && decryptError.isEmpty()) {
        SecureJoin::Incoming in;
        in.headers = headers;
        in.gossip = encrypted ? content.headers.all("Autocrypt-Gossip") : QList<QByteArray>();
        in.from = from.addr;
        in.encrypted = encrypted;
        in.validSigners = validSigners;
        in.date = date;
        // A step that cannot be answered (a key that will not encrypt) must
        // not stop the message being stored: it would be fetched forever.
        try {
            SecureJoin::receive(ctx, in);
        } catch (const std::exception &e) {
            qCWarning(lcMail) << "SecureJoin step from" << from.addr << "not answered:" << e.what();
        }
    }

    ctx.db.run("INSERT INTO messages(message_id, direction, state, subject, from_addr, from_name, reply_to, date, "
               "received_at, in_reply_to, body_text, body_html, preview, importance, encrypted, signed_fpr, "
               "verified, has_attachments, raw_blob, raw_expires_at, hidden, unread) "
               "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
               messageId, int(fromSelf ? Direction::Outgoing : Direction::Incoming),
               int(fromSelf ? MessageState::Sent : MessageState::Received), subject, from.addr, from.name,
               replyTo.isEmpty() ? std::optional<QString>() : nullIfEmpty(replyTo.first().addr), date.toMSecsSinceEpoch(), now, inReplyTo,
               body.text, nullIfEmpty(body.html), makePreview(body.text), importance,
               encrypted, nullIfEmpty(signedFpr), verified, !body.attachments.isEmpty(),
               nullIfEmpty(rawBlob), rawExpires, hidden, !fromSelf);
    const qint64 id = ctx.db.lastInsertId();
    result.msgId = id;

    auto addRecipients = [&](const QList<mime::Address> &list, RecipientKind kind) {
        int idx = 0;
        for (const mime::Address &a : list)
            ctx.db.run("INSERT OR IGNORE INTO recipients(msg_id, kind, idx, addr, name) VALUES(?, ?, ?, ?, ?)", id,
                       int(kind), idx++, a.addr, a.name);
    };
    addRecipients(to, RecipientKind::To);
    addRecipients(cc, RecipientKind::Cc);
    for (qsizetype i = 0; i < references.size(); ++i)
        ctx.db.run("INSERT INTO msg_refs(msg_id, idx, ref) VALUES(?, ?, ?)", id, int(i), references[i]);
    for (qsizetype i = 0; i < body.attachments.size(); ++i) {
        const Content::Attachment &a = body.attachments[i];
        ctx.db.run("INSERT INTO attachments(msg_id, idx, filename, mime_type, size, blob, content_id) "
                   "VALUES(?, ?, ?, ?, ?, ?, ?)",
                   id, int(i), a.filename, a.mimeType, qint64(a.data.size()), attachmentBlobs[i],
                   nullIfEmpty(a.contentId));
    }
    Threading::assign(ctx, id, messageId, inReplyTo, references, date.toMSecsSinceEpoch());
    Search::index(ctx, id);

    // Where it lands. The blocklist has the last word; under strict mode
    // cleartext goes to Trash; otherwise strangers wait in Unverified.
    if (!fromSelf && !hidden) {
        const auto mode = EncryptionMode(ctx.config.getInt(cfg::EncryptionMode));
        if (Contacts::isBlocked(ctx, from.addr)) {
            Organize::trash(ctx, id, TrashReason::Blocked);
            result.trashed = true;
        } else if (mode == EncryptionMode::Strict && !encrypted) {
            Organize::trash(ctx, id, TrashReason::Cleartext);
            result.trashed = true;
        } else if (ctx.config.getBool(cfg::Gating) && !Contacts::isTrusted(ctx, from.addr)) {
            Organize::hold(ctx, id);
            result.held = true;
        }
    }
    // Device-sync mail reaches every device from the server; passing it on
    // would only be more ops to sync.
    if (!hidden)
        ctx.ops.record(op::MessageAdded, {{QStringLiteral("mid"), messageId}, {QStringLiteral("raw"), rawBlob}});
    // What other devices already did to it, if their ops came first.
    sync::OpApply::replayFor(ctx, messageId);
    tx.commit();
    qCInfo(lcMail) << "stored message" << id << (encrypted ? "encrypted" : "cleartext") << "from" << from.addr
                   << (result.held ? "(held)" : "") << (result.trashed ? "(trashed)" : "");
    return result;
}

} // namespace e3::mail
