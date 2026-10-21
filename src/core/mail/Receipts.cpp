// SPDX-License-Identifier: MPL-2.0
#include "Receipts.h"

#include "Compose.h"
#include "Contacts.h"
#include "Keyring.h"
#include "crypto/Autocrypt.h"
#include "crypto/Pgp.h"
#include "mime/Builder.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"
#include "util/Log.h"

#include <QTimeZone>

namespace e3::mail {

namespace {

// The report itself, or under the RFC 9788 wrapper an encrypted one has.
const mime::Part *findReport(const mime::Part &p, int depth = 0)
{
    if (p.mimeType == "multipart/report"
        && p.typeParams.value("report-type").toLower() == QLatin1String("disposition-notification"))
        return &p;
    if (depth >= 3 || !p.isMultipart())
        return nullptr;
    for (const mime::Part &c : p.children) {
        if (const mime::Part *r = findReport(c, depth + 1))
            return r;
    }
    return nullptr;
}

} // namespace

QString Receipts::requestedBy(const mime::HeaderList &headers)
{
    QByteArray v = headers.raw("Disposition-Notification-To");
    if (v.trimmed().isEmpty())
        v = headers.raw("Chat-Disposition-Notification-To");
    return mime::parseAddressList(v).value(0).addr;
}

bool Receipts::sendIfDue(MailContext &ctx, qint64 msgId)
{
    Statement st(ctx.db, "SELECT message_id, from_addr, mdn_to, encrypted, subject FROM messages "
                         "WHERE id=? AND direction=? AND hidden=0 AND mdn_to IS NOT NULL");
    st.bindAll(msgId, int(Direction::Incoming));
    if (!st.step())
        return false;
    const QString mid = st.text(0);
    const QString from = mime::normalizeAddr(st.text(1));
    const QString to = mime::normalizeAddr(st.text(2));
    const bool wasEncrypted = st.integer(3);
    // Only to the sender themselves: a receipt sent elsewhere would tell a
    // third party that this mail was read (RFC 8098, section 2.1).
    if (to.isEmpty() || to != from)
        return false;
    if (ctx.config.get(cfg::MdnSend) != QLatin1String("trusted") || !Contacts::isTrusted(ctx, from))
        return false;
    if (ctx.db.queryInt("SELECT 1 FROM held WHERE msg_id=?", msgId)
        || ctx.db.queryInt("SELECT 1 FROM trashed WHERE msg_id=?", msgId))
        return false;
    // Once, whichever device read it first.
    if (ctx.db.queryInt("SELECT 1 FROM ops WHERE mkey=?", OpLog::mergeKey(op::ReceiptSent, {{QStringLiteral("mid"), mid}})))
        return false;

    const QString self = ctx.config.get(cfg::Addr);
    const QString selfFpr = Keyring::ensureSelfKey(ctx, self);
    mime::OutgoingMail mail;
    mail.from = {ctx.config.get(cfg::DisplayName), self};
    mail.to = {{QString(), from}};
    mail.subject = QStringLiteral("Receipt Notification");
    mail.date = QDateTime::fromSecsSinceEpoch(QDateTime::currentSecsSinceEpoch(), QTimeZone::UTC);
    mail.messageId = mime::generateMessageId(self);
    mail.inReplyTo = mid;
    mail.references = {mid};
    mail.text = QStringLiteral("This is a receipt notification.");
    mail.extraHeaders.append(
        {"Autocrypt", crypto::Autocrypt::format(self, ctx.pgp.exportAutocrypt(selfFpr, self), true)});

    // Encrypted when what it answers was, so the receipt says no more to the
    // server than the message did.
    QString peerFpr;
    if (wasEncrypted) {
        const auto c = Contacts::get(ctx, from);
        if (c && !c->fingerprint.isEmpty() && Keyring::ensure(ctx, c->fingerprint) && ctx.pgp.canEncryptTo(c->fingerprint))
            peerFpr = c->fingerprint;
        else
            return false; // never answer encrypted mail in cleartext
    }
    QByteArray raw;
    if (!peerFpr.isEmpty()) {
        const QByteArray inner = mime::Builder::receiptEntity(mail, mid, self, true);
        const QByteArray armored = ctx.pgp.encrypt(inner, {peerFpr, selfFpr}, selfFpr);
        const QString hidden = QStringLiteral("...");
        raw = mime::Builder::headerBlock(mail, &hidden) + mime::Builder::encryptedBody(armored);
    } else {
        raw = mime::Builder::headerBlock(mail) + mime::Builder::receiptEntity(mail, mid, self, false);
    }
    Compose::queueMachineMail(ctx, mail, raw, from, !peerFpr.isEmpty());
    ctx.ops.record(op::ReceiptSent, {{QStringLiteral("mid"), mid}});
    qCInfo(lcMail) << "queued a read receipt to" << from;
    return true;
}

bool Receipts::isReport(const mime::Part &content)
{
    return findReport(content) != nullptr;
}

bool Receipts::receive(MailContext &ctx, const mime::Part &content, const QString &from, const QDateTime &date)
{
    const mime::Part *report = findReport(content);
    if (!report)
        return false;
    for (const mime::Part &c : report->children) {
        if (c.mimeType != "message/disposition-notification")
            continue;
        const mime::Part fields = mime::parse(c.body + "\r\n");
        if (!QString::fromUtf8(fields.headers.raw("Disposition")).contains(QLatin1String("displayed"), Qt::CaseInsensitive))
            return false;
        const QString original = mime::parseMessageIds(fields.headers.raw("Original-Message-ID")).value(0);
        const auto id = ctx.db.queryInt("SELECT id FROM messages WHERE message_id=? AND direction=?", original,
                                        int(Direction::Outgoing));
        const QString reader = mime::normalizeAddr(from);
        // Only someone the message went to can say they read it.
        if (!id || !ctx.db.queryInt("SELECT 1 FROM recipients WHERE msg_id=? AND lower(addr)=?", *id, reader))
            return false;
        ctx.db.run("INSERT OR IGNORE INTO receipts(msg_id, addr, at) VALUES(?, ?, ?)", *id, reader,
                   date.toMSecsSinceEpoch());
        return true;
    }
    return false;
}

} // namespace e3::mail
