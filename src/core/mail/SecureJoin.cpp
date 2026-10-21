// SPDX-License-Identifier: MPL-2.0
#include "SecureJoin.h"

#include "Compose.h"
#include "Contacts.h"
#include "Keyring.h"
#include "Organize.h"
#include "crypto/Autocrypt.h"
#include "crypto/Pgp.h"
#include "mime/Builder.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"
#include "util/Log.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QRandomGenerator>
#include <QTimeZone>
#include <QUrl>
#include <QUuid>

namespace e3::mail {

namespace {

constexpr char kScheme[] = "OPENPGP4FPR:";
// Delta Chat's invite links: `https://i.delta.chat/#FPR&a=...`.
constexpr char kLink[] = "https://i.delta.chat/#";
constexpr char kLinkNoSlash[] = "https://i.delta.chat#";

constexpr char kStep[] = "Secure-Join";
constexpr char kInvitenumber[] = "Secure-Join-Invitenumber";
constexpr char kAuth[] = "Secure-Join-Auth";
constexpr char kFingerprint[] = "Secure-Join-Fingerprint";

// 18 random bytes, base64url: what Delta Chat accepts as a token (11 to 32
// characters of that alphabet).
QString newToken()
{
    QByteArray bytes(18, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(bytes.data()), bytes.size() / 4);
    for (qsizetype i = bytes.size() / 4 * 4; i < bytes.size(); ++i)
        bytes[i] = char(QRandomGenerator::system()->bounded(256));
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool validToken(const QString &s)
{
    if (s.size() <= 10 || s.size() > 32)
        return false;
    for (const QChar c : s) {
        if (!(c.isLetterOrNumber() && c.unicode() < 128) && c != u'-' && c != u'_')
            return false;
    }
    return true;
}

bool validFingerprint(const QString &fpr)
{
    if (fpr.size() != 40 && fpr.size() != 64)
        return false;
    for (const QChar c : fpr) {
        if (!((c >= u'0' && c <= u'9') || (c >= u'A' && c <= u'F')))
            return false;
    }
    return true;
}

// As Delta Chat encodes them: everything but letters, digits, '.' and '_'
// percent-encoded, and spaces as '+'.
QString encodeParam(const QString &s)
{
    QString out = QString::fromLatin1(s.toUtf8().toPercentEncoding("._"));
    out.replace(QStringLiteral("%20"), QStringLiteral("+"));
    return out;
}

QString decodeParam(QString s)
{
    s.replace(u'+', QStringLiteral("%20"));
    return QUrl::fromPercentEncoding(s.toUtf8());
}

QString selfAddr(MailContext &ctx)
{
    return ctx.config.get(cfg::Addr);
}

QJsonObject inviteOp(const QString &invitenumber, const QString &authcode, qint64 createdAt, bool revoked)
{
    return {{QStringLiteral("i"), invitenumber},
            {QStringLiteral("s"), authcode},
            {QStringLiteral("at"), double(createdAt)},
            {QStringLiteral("revoked"), revoked}};
}

void writeJoin(MailContext &ctx, const SecureJoin::Invite &inv, SecureJoin::JoinState state, const QString &error,
               qint64 startedAt)
{
    const QJsonObject payload{{QStringLiteral("addr"), mime::normalizeAddr(inv.addr)},
                              {QStringLiteral("name"), inv.name},
                              {QStringLiteral("fpr"), inv.fpr},
                              {QStringLiteral("i"), inv.invitenumber},
                              {QStringLiteral("s"), inv.authcode},
                              {QStringLiteral("state"), int(state)},
                              {QStringLiteral("error"), error},
                              {QStringLiteral("at"), double(startedAt)}};
    SecureJoin::applyJoin(ctx, payload);
    ctx.ops.record(op::SecureJoinJoin, payload);
}

std::optional<SecureJoin::Invite> pendingJoin(MailContext &ctx, const QString &addr, SecureJoin::JoinState *state,
                                              qint64 *startedAt)
{
    Statement st(ctx.db, "SELECT addr, name, fpr, invitenumber, authcode, state, started_at FROM sj_joins WHERE addr=?");
    st.bind(1, mime::normalizeAddr(addr));
    if (!st.step())
        return std::nullopt;
    SecureJoin::Invite inv{st.text(2), st.text(0), st.text(1), st.text(3), st.text(4)};
    *state = SecureJoin::JoinState(st.int64(5));
    *startedAt = st.int64(6);
    return inv;
}

// Queues one handshake message, hidden: never in a list, never synced as a
// message. Inside the caller's transaction.
void send(MailContext &ctx, const QString &to, const QString &step,
          const QList<QPair<QByteArray, QByteArray>> &stepHeaders, const QString &peerFpr)
{
    const QString self = selfAddr(ctx);
    const QString selfFpr = Keyring::ensureSelfKey(ctx, self);
    mime::OutgoingMail mail;
    mail.from = {ctx.config.get(cfg::DisplayName), self};
    mail.to = {{QString(), to}};
    mail.subject = QStringLiteral("Secure-Join");
    mail.date = QDateTime::fromSecsSinceEpoch(QDateTime::currentSecsSinceEpoch(), QTimeZone::UTC);
    const QString domain = self.contains(u'@') ? self.section(u'@', -1) : QStringLiteral("e3mail.invalid");
    mail.messageId = QUuid::createUuid().toString(QUuid::WithoutBraces) + u'@' + domain;
    mail.text = QStringLiteral("Secure-Join: ") + step;
    mail.extraHeaders.append(
        {"Autocrypt", crypto::Autocrypt::format(self, ctx.pgp.exportAutocrypt(selfFpr, self), true)});

    QByteArray raw;
    const bool encrypt = !peerFpr.isEmpty();
    if (encrypt) {
        // The step and its secrets only inside the encryption. Gossiping the
        // peer's own key back to them tells them the message was meant for
        // them (Delta Chat checks it against surreptitious forwarding).
        mail.innerHeaders.append({kStep, step.toUtf8()});
        mail.innerHeaders.append(stepHeaders);
        mail.extraHeaders.append(
            {"Autocrypt-Gossip", crypto::Autocrypt::format(to, ctx.pgp.exportAutocrypt(peerFpr, to), false)});
        const QByteArray inner = mime::Builder::contentEntity(mail, true);
        const QByteArray armored = ctx.pgp.encrypt(inner, {peerFpr, selfFpr}, selfFpr);
        const QString hidden = QStringLiteral("...");
        raw = mime::Builder::headerBlock(mail, &hidden) + mime::Builder::encryptedBody(armored);
    } else {
        mail.extraHeaders.append({kStep, step.toUtf8()});
        mail.extraHeaders.append(stepHeaders);
        raw = mime::Builder::build(mail);
    }

    const qint64 now = mail.date.toMSecsSinceEpoch();
    ctx.db.run("INSERT INTO messages(message_id, direction, state, subject, from_addr, date, received_at, encrypted, "
               "signed_fpr, hidden) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, 1)",
               mail.messageId, int(Direction::Outgoing), int(MessageState::Pending), mail.subject, self, now, now,
               encrypt, encrypt ? selfFpr : QString());
    const qint64 id = ctx.db.lastInsertId();
    ctx.db.run("INSERT INTO outbox(msg_id, raw, mail_from, rcpts) VALUES(?, ?, ?, ?)", id, raw, self, to);
    qCInfo(lcMail) << "queued SecureJoin" << step << "to" << to;
}

// Was it encrypted, signed by `fpr`, and did it gossip our own key back to
// us? Anything less is not a step of a handshake with that key.
bool authentic(MailContext &ctx, const SecureJoin::Incoming &m, const QString &fpr)
{
    if (!m.encrypted || fpr.isEmpty() || !m.validSigners.contains(fpr))
        return false;
    const QString self = mime::normalizeAddr(selfAddr(ctx));
    const QString selfFpr = Keyring::selfFingerprint(ctx);
    for (const QByteArray &g : m.gossip) {
        const auto gossip = crypto::Autocrypt::parse(g);
        if (!gossip || mime::normalizeAddr(gossip->addr) != self)
            continue;
        try {
            if (ctx.pgp.import(gossip->keydata).contains(selfFpr))
                return true;
        } catch (const crypto::PgpError &) {
        }
    }
    qCWarning(lcMail) << "SecureJoin step without our own key gossiped back; ignored";
    return false;
}

QString header(const SecureJoin::Incoming &m, const char *name)
{
    return QString::fromUtf8(m.headers.raw(name).trimmed());
}

} // namespace

QString SecureJoin::invite(MailContext &ctx)
{
    const QString addr = selfAddr(ctx);
    const QString fpr = Keyring::ensureSelfKey(ctx, addr);
    Statement st(ctx.db, "SELECT invitenumber, authcode FROM sj_invites WHERE revoked=0 ORDER BY created_at DESC LIMIT 1");
    Invite inv{fpr, addr, ctx.config.get(cfg::DisplayName), {}, {}};
    if (st.step()) {
        inv.invitenumber = st.text(0);
        inv.authcode = st.text(1);
        return format(inv);
    }
    inv.invitenumber = newToken();
    inv.authcode = newToken();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    Transaction tx(ctx.db);
    const QJsonObject payload = inviteOp(inv.invitenumber, inv.authcode, now, false);
    applyInvite(ctx, payload);
    ctx.ops.record(op::SecureJoinInvite, payload);
    tx.commit();
    return format(inv);
}

QString SecureJoin::renewInvite(MailContext &ctx)
{
    {
        Transaction tx(ctx.db);
        Statement st(ctx.db, "SELECT invitenumber, authcode, created_at FROM sj_invites WHERE revoked=0");
        QList<QJsonObject> revoked;
        while (st.step())
            revoked.append(inviteOp(st.text(0), st.text(1), st.int64(2), true));
        for (const QJsonObject &p : std::as_const(revoked)) {
            applyInvite(ctx, p);
            ctx.ops.record(op::SecureJoinInvite, p);
        }
        tx.commit();
    }
    return invite(ctx);
}

std::optional<SecureJoin::Invite> SecureJoin::parse(const QString &text)
{
    QString s = text.trimmed();
    QString payload;
    if (s.startsWith(QLatin1String(kScheme), Qt::CaseInsensitive)) {
        payload = s.mid(int(qstrlen(kScheme)));
    } else {
        for (const char *link : {kLink, kLinkNoSlash}) {
            if (s.startsWith(QLatin1String(link), Qt::CaseInsensitive)) {
                payload = s.mid(int(qstrlen(link)));
                const qsizetype amp = payload.indexOf(u'&');
                if (amp >= 0)
                    payload[amp] = u'#';
                break;
            }
        }
        if (payload.isEmpty())
            return std::nullopt;
    }
    // Some phones percent-encode the '#'.
    qsizetype hash = payload.indexOf(u'#');
    qsizetype skip = 1;
    if (hash < 0 && (hash = payload.indexOf(QLatin1String("%23"))) >= 0)
        skip = 3;
    const QString fpr = crypto::Pgp::normalizeFingerprint(hash < 0 ? payload : payload.left(hash));
    if (!validFingerprint(fpr))
        return std::nullopt;
    Invite inv;
    inv.fpr = fpr;
    if (hash >= 0) {
        for (const QString &kv : payload.mid(hash + skip).split(u'&', Qt::SkipEmptyParts)) {
            const qsizetype eq = kv.indexOf(u'=');
            if (eq <= 0)
                continue;
            const QString key = kv.left(eq);
            const QString value = kv.mid(eq + 1);
            if (key == QLatin1String("a"))
                inv.addr = decodeParam(value).trimmed();
            else if (key == QLatin1String("n"))
                inv.name = decodeParam(value).trimmed();
            else if (key == QLatin1String("i"))
                inv.invitenumber = value;
            else if (key == QLatin1String("s"))
                inv.authcode = value;
            else if (key == QLatin1String("g") || key == QLatin1String("x") || key == QLatin1String("b")
                     || key == QLatin1String("j"))
                return std::nullopt; // a group or a channel: e3mail has neither
        }
    }
    // A Delta Chat name cut short ends in '_'.
    if (inv.name.endsWith(u'_'))
        inv.name = inv.name.chopped(1) + QChar(0x2026);
    if (!inv.addr.contains(u'@') || !validToken(inv.invitenumber) || !validToken(inv.authcode))
        return std::nullopt;
    return inv;
}

QString SecureJoin::format(const Invite &inv)
{
    QString out = QLatin1String(kScheme) + inv.fpr + QStringLiteral("#a=") + encodeParam(inv.addr);
    if (!inv.name.isEmpty())
        out += QStringLiteral("&n=") + encodeParam(inv.name.left(25));
    out += QStringLiteral("&i=") + inv.invitenumber + QStringLiteral("&s=") + inv.authcode;
    return out;
}

void SecureJoin::join(MailContext &ctx, const Invite &invite)
{
    const QString self = selfAddr(ctx);
    const QString selfFpr = Keyring::ensureSelfKey(ctx, self);
    if (invite.fpr == selfFpr || mime::normalizeAddr(invite.addr) == mime::normalizeAddr(self))
        throw ComposeError(QCoreApplication::translate("SecureJoin", "This is your own code. Scan the other person's code instead."));
    if (Contacts::isBlocked(ctx, invite.addr))
        throw ComposeError(QCoreApplication::translate("SecureJoin", "%1 is blocked. Unblock them first.").arg(invite.addr));

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    Transaction tx(ctx.db);
    Contacts::touch(ctx, invite.addr, invite.name, ContactOrigin::Manual);
    // Already holding the key the invite names: straight to the token, as
    // Delta Chat does. Otherwise ask for the key first.
    if (Keyring::ensure(ctx, invite.fpr) && ctx.pgp.canEncryptTo(invite.fpr)) {
        Contacts::setVerified(ctx, invite.addr, invite.fpr);
        Organize::releaseHeld(ctx, invite.addr);
        send(ctx, invite.addr, QStringLiteral("vc-request-with-auth"),
             {{kAuth, invite.authcode.toUtf8()}, {kFingerprint, selfFpr.toUtf8()}}, invite.fpr);
        writeJoin(ctx, invite, JoinState::AuthSent, QString(), now);
    } else {
        send(ctx, invite.addr, QStringLiteral("vc-request"), {{kInvitenumber, invite.invitenumber.toUtf8()}}, QString());
        writeJoin(ctx, invite, JoinState::Requested, QString(), now);
    }
    tx.commit();
}

std::optional<SecureJoin::Join> SecureJoin::joinFor(MailContext &ctx, const QString &addr)
{
    Statement st(ctx.db, "SELECT addr, name, fpr, state, error, started_at FROM sj_joins WHERE addr=?");
    st.bind(1, mime::normalizeAddr(addr));
    if (!st.step())
        return std::nullopt;
    return Join{st.text(0), st.text(1), st.text(2), JoinState(st.int64(3)), st.text(4), st.int64(5)};
}

QList<SecureJoin::Join> SecureJoin::joins(MailContext &ctx)
{
    QList<Join> out;
    Statement st(ctx.db, "SELECT addr, name, fpr, state, error, started_at FROM sj_joins ORDER BY started_at DESC");
    while (st.step())
        out.append({st.text(0), st.text(1), st.text(2), JoinState(st.int64(3)), st.text(4), st.int64(5)});
    return out;
}

bool SecureJoin::isHandshake(const mime::HeaderList &headers)
{
    return headers.has(kStep) || headers.has(kInvitenumber);
}

bool SecureJoin::receive(MailContext &ctx, const Incoming &m)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m.date.isValid() && m.date.toMSecsSinceEpoch() < now - kAnswerWithinMs) {
        qCInfo(lcMail) << "old SecureJoin message; not answered";
        return false;
    }
    const QString from = mime::normalizeAddr(m.from);
    if (from.isEmpty() || from == mime::normalizeAddr(selfAddr(ctx)) || Contacts::isBlocked(ctx, from))
        return false;
    const QString step = header(m, kStep);
    const QString selfFpr = Keyring::selfFingerprint(ctx);

    // Alice: someone scanned our invite and asks for our key. The request is
    // cleartext; it carried the joiner's key in its Autocrypt header, which
    // Ingest has learned. Answering reveals nothing but that key exchange.
    if (const QString number = header(m, kInvitenumber); !number.isEmpty()) {
        if (!ctx.db.queryInt("SELECT 1 FROM sj_invites WHERE invitenumber=? AND revoked=0", number)) {
            qCWarning(lcMail) << "SecureJoin request with an unknown invite from" << from;
            return false;
        }
        const auto c = Contacts::get(ctx, from);
        if (!c || c->fingerprint.isEmpty() || !Keyring::ensure(ctx, c->fingerprint)
            || !ctx.pgp.canEncryptTo(c->fingerprint)) {
            qCWarning(lcMail) << "SecureJoin request without a usable key from" << from;
            return false;
        }
        const QString prefix = step.startsWith(QLatin1String("vg-")) ? QStringLiteral("vg") : QStringLiteral("vc");
        send(ctx, from, prefix + QStringLiteral("-auth-required"), {}, c->fingerprint);
        return true;
    }

    if (step == QLatin1String("vc-auth-required")) {
        // Bob: the inviter answered. It counts only if it came from the key
        // the invite named, which makes that key verified.
        JoinState state;
        qint64 startedAt = 0;
        const auto inv = pendingJoin(ctx, from, &state, &startedAt);
        if (!inv || state != JoinState::Requested || !authentic(ctx, m, inv->fpr))
            return false;
        Contacts::setVerified(ctx, from, inv->fpr);
        Organize::releaseHeld(ctx, from);
        send(ctx, from, QStringLiteral("vc-request-with-auth"),
             {{kAuth, inv->authcode.toUtf8()}, {kFingerprint, selfFpr.toUtf8()}}, inv->fpr);
        writeJoin(ctx, *inv, JoinState::AuthSent, QString(), startedAt);
        return true;
    }

    if (step == QLatin1String("vc-request-with-auth")) {
        // Alice: the joiner proves they saw the invite, signed by the key
        // they name. That key becomes verified.
        const QString fpr = crypto::Pgp::normalizeFingerprint(header(m, kFingerprint));
        const QString auth = header(m, kAuth);
        if (!authentic(ctx, m, fpr)) {
            qCWarning(lcMail) << "SecureJoin token not encrypted or not signed by the key it names, from" << from;
            return false;
        }
        if (auth.isEmpty() || !ctx.db.queryInt("SELECT 1 FROM sj_invites WHERE authcode=? AND revoked=0", auth)) {
            qCWarning(lcMail) << "SecureJoin token that is not ours, from" << from;
            return false;
        }
        Contacts::touch(ctx, from, QString(), ContactOrigin::Manual);
        Contacts::setVerified(ctx, from, fpr);
        Organize::releaseHeld(ctx, from);
        send(ctx, from, QStringLiteral("vc-contact-confirm"), {}, fpr);
        qCInfo(lcMail) << "verified" << from << "by SecureJoin";
        return true;
    }

    if (step == QLatin1String("vc-contact-confirm")) {
        JoinState state;
        qint64 startedAt = 0;
        const auto inv = pendingJoin(ctx, from, &state, &startedAt);
        if (!inv || state == JoinState::Done || !authentic(ctx, m, inv->fpr))
            return false;
        if (const auto c = Contacts::get(ctx, from); !c || !c->verified || c->fingerprint != inv->fpr)
            Contacts::setVerified(ctx, from, inv->fpr);
        writeJoin(ctx, *inv, JoinState::Done, QString(), startedAt);
        qCInfo(lcMail) << "verified" << from << "by SecureJoin, confirmed";
        return true;
    }
    // Group steps, deprecated acknowledgements, anything newer: hidden and
    // left alone.
    return false;
}

int SecureJoin::expire(MailContext &ctx, qint64 now)
{
    QList<std::pair<Invite, qint64>> stale;
    Statement st(ctx.db, "SELECT addr, name, fpr, invitenumber, authcode, started_at FROM sj_joins "
                         "WHERE state IN (1, 2) AND started_at < ?");
    st.bind(1, now - kJoinTimeoutMs);
    while (st.step())
        stale.append({Invite{st.text(2), st.text(0), st.text(1), st.text(3), st.text(4)}, st.int64(5)});
    for (const auto &[inv, at] : std::as_const(stale))
        writeJoin(ctx, inv, JoinState::Failed, QStringLiteral("timeout"), at);
    return int(stale.size());
}

void SecureJoin::applyInvite(MailContext &ctx, const QJsonObject &p)
{
    const QString number = p.value(QLatin1String("i")).toString();
    const QString auth = p.value(QLatin1String("s")).toString();
    if (!validToken(number) || !validToken(auth))
        return;
    ctx.db.run("INSERT INTO sj_invites(invitenumber, authcode, created_at, revoked) VALUES(?, ?, ?, ?) "
               "ON CONFLICT(invitenumber) DO UPDATE SET revoked=excluded.revoked",
               number, auth, qint64(p.value(QLatin1String("at")).toDouble()),
               p.value(QLatin1String("revoked")).toBool());
}

void SecureJoin::applyJoin(MailContext &ctx, const QJsonObject &p)
{
    auto s = [&p](const char *k) { return p.value(QLatin1String(k)).toString(); };
    const QString addr = mime::normalizeAddr(s("addr"));
    if (!addr.contains(u'@'))
        return;
    ctx.db.run("INSERT OR REPLACE INTO sj_joins(addr, name, fpr, invitenumber, authcode, state, error, started_at) "
               "VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
               addr, s("name"), s("fpr"), s("i"), s("s"), p.value(QLatin1String("state")).toInt(), s("error"),
               qint64(p.value(QLatin1String("at")).toDouble()));
}

} // namespace e3::mail
