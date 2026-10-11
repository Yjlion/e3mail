// SPDX-License-Identifier: MPL-2.0
#include "MailboxFallback.h"

#include "OpApply.h"
#include "SyncSession.h"
#include "crypto/Pgp.h"
#include "mail/Keyring.h"
#include "mime/Builder.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"
#include "util/Log.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <QUuid>

#include <limits>

namespace e3::sync {

namespace {
// Device-local bookkeeping, never synced.
constexpr char kMailedUpTo[] = "p2p_mailed_seq"; // own ops mailed so far
constexpr char kOwed[] = "p2p_mail_owed";        // the rest of a full batch is due
constexpr char kFull[] = "p2p_mail_full";        // the last mail held a full batch
}

qint64 MailboxFallback::queueIfDue(MailContext &ctx, const QSet<QString> &connected)
{
    const QString self = ctx.ops.deviceId();
    const qint64 mine = ctx.db.queryInt("SELECT max(seq) FROM ops WHERE device=?", self).value_or(0);
    // The least any absent device is known to hold of ours.
    qint64 from = -1;
    Statement st(ctx.db, "SELECT id FROM devices");
    while (st.step()) {
        const QString device = st.text(0);
        if (connected.contains(device))
            continue;
        const qint64 has = SyncSession::storedHave(ctx, device).value(self);
        from = from < 0 ? has : qMin(from, has);
    }
    if (from < 0)
        return 0; // every device is here, or there are none
    // News is any op since the last mail, except having fetched a sync mail:
    // that ack would itself be mailed, fetched and acked, every few minutes,
    // forever. It goes along with the next real change.
    const qint64 since = qMax<qint64>(ctx.config.getInt(kMailedUpTo), from);
    const bool news = ctx.db.queryInt(
        "SELECT 1 FROM ops o WHERE o.device=? AND o.seq>? AND (o.kind != 'server.ack' OR NOT EXISTS ("
        "SELECT 1 FROM server_msgs s JOIN messages m ON m.id = s.msg_id "
        "WHERE s.remote_key = json_extract(o.payload, '$.key') AND m.hidden = 1)) LIMIT 1",
        self, since).has_value();
    if (!news && !ctx.config.getBool(kOwed))
        return 0;

    // Our own ops from what the furthest-behind device has: each mail stands
    // on its own, whichever arrives first.
    OpApply::Versions have = OpApply::versions(ctx.db);
    for (auto it = have.begin(); it != have.end(); ++it)
        it.value() = std::numeric_limits<qint64>::max();
    have.insert(self, from);
    QJsonArray ops;
    qint64 last = from;
    for (const Op &o : OpApply::missing(ctx.db, have, kBatch)) {
        ops.append(SyncSession::opToWire(o));
        last = qMax(last, o.seq);
    }
    QJsonObject holds;
    const OpApply::Versions mineAll = OpApply::versions(ctx.db);
    for (auto it = mineAll.cbegin(); it != mineAll.cend(); ++it)
        holds.insert(it.key(), double(it.value()));
    const QByteArray payload = QJsonDocument(QJsonObject{{QStringLiteral("v"), 1},
                                                         {QStringLiteral("d"), self},
                                                         {QStringLiteral("ops"), ops},
                                                         // full: answer, so the rest can follow
                                                         {QStringLiteral("more"), ops.size() >= kBatch},
                                                         {QStringLiteral("have"), holds}})
                                   .toJson(QJsonDocument::Compact);

    const QString addr = ctx.config.get(cfg::Addr);
    const QString fpr = mail::Keyring::ensureSelfKey(ctx, addr);
    mime::OutgoingMail mail;
    mail.from = {QString(), addr};
    mail.to = {{QString(), addr}};
    mail.subject = QStringLiteral("e3mail device sync");
    mail.date = QDateTime::fromSecsSinceEpoch(QDateTime::currentSecsSinceEpoch(), QTimeZone::UTC);
    mail.messageId = QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral("@e3mail.invalid");
    mail.text = QString::fromUtf8(payload);
    const QByteArray armored = ctx.pgp.encrypt(mime::Builder::contentEntity(mail), {fpr}, fpr);
    mail.extraHeaders.append({QByteArray(kHeader), QByteArray("1")});
    const QString subject = QStringLiteral("...");
    const QByteArray raw = mime::Builder::headerBlock(mail, &subject) + mime::Builder::encryptedBody(armored);

    const qint64 now = mail.date.toMSecsSinceEpoch();
    Transaction tx(ctx.db);
    ctx.db.run("INSERT INTO messages(message_id, direction, state, subject, from_addr, date, received_at, encrypted, "
               "signed_fpr, hidden) VALUES(?, ?, ?, ?, ?, ?, ?, 1, ?, 1)",
               mail.messageId, int(Direction::Outgoing), int(MessageState::Pending), mail.subject, addr, now, now, fpr);
    const qint64 id = ctx.db.lastInsertId();
    ctx.db.run("INSERT INTO outbox(msg_id, raw, mail_from, rcpts) VALUES(?, ?, ?, ?)", id, raw, addr, addr);
    ctx.config.set(kMailedUpTo, QString::number(qMax(mine, last)));
    ctx.config.set(kOwed, QStringLiteral("0"));
    ctx.config.set(kFull, ops.size() >= kBatch ? QStringLiteral("1") : QStringLiteral("0"));
    tx.commit();
    qCInfo(lcSync) << "mailed" << ops.size() << "ops for devices that are away";
    return id;
}

bool MailboxFallback::receive(MailContext &ctx, const QString &body, const QStringList &validSigners)
{
    const QJsonObject msg = QJsonDocument::fromJson(body.trimmed().toUtf8()).object();
    const QString sender = msg.value(QLatin1String("d")).toString();
    if (msg.value(QLatin1String("v")).toInt() != 1 || sender.isEmpty())
        return false;
    // Only this account's own key speaks for its devices.
    if (!validSigners.contains(mail::Keyring::selfFingerprint(ctx))) {
        qCWarning(lcSync) << "ignoring device-sync mail not signed by this account";
        return false;
    }
    const QString self = ctx.ops.deviceId();
    if (sender == self || !ctx.db.queryInt("SELECT 1 FROM devices WHERE id=?", sender))
        return false;
    if (msg.value(QLatin1String("more")).toBool())
        ctx.config.set(kOwed, QStringLiteral("1"));
    QList<Op> ops;
    for (const QJsonValue &v : msg.value(QLatin1String("ops")).toArray())
        ops.append(SyncSession::opFromWire(v.toObject()));
    const OpApply::Result r = OpApply::apply(ctx, ops);
    // What the sender holds now, ours included.
    OpApply::Versions have;
    const QJsonObject h = msg.value(QLatin1String("have")).toObject();
    for (auto it = h.begin(); it != h.end(); ++it)
        have.insert(it.key(), qint64(it.value().toDouble()));
    const qint64 before = SyncSession::storedHave(ctx, sender).value(self);
    SyncSession::storeHave(ctx, sender, have);
    const qint64 mine = ctx.db.queryInt("SELECT max(seq) FROM ops WHERE device=?", self).value_or(0);
    // It holds more of ours but not all (a batch was full): send the rest,
    // from where it is. Its news needs no answer of its own; what this
    // device holds rides along with its next mail.
    if (ctx.config.getBool(kFull) && have.value(self) > before && have.value(self) < mine) {
        ctx.config.set(kOwed, QStringLiteral("1"));
        ctx.config.set(kMailedUpTo, QString::number(have.value(self)));
    }
    qCInfo(lcSync) << "applied" << r.accepted << "ops mailed by" << sender;
    return true;
}

} // namespace e3::sync
