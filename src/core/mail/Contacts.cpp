// SPDX-License-Identifier: MPL-2.0
#include "Contacts.h"

#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"

#include <QJsonObject>

namespace e3::mail {

namespace {

ContactInfo read(MailContext &ctx, Statement &st)
{
    ContactInfo c;
    c.id = st.int64(0);
    c.addr = st.text(1);
    c.name = st.text(2);
    c.origin = ContactOrigin(st.integer(3));
    c.fingerprint = st.text(4);
    c.verified = st.integer(5);
    c.preferEncrypt = st.integer(6);
    if (!st.isNull(7))
        c.lastSeen = QDateTime::fromMSecsSinceEpoch(st.int64(7));
    if (!st.isNull(8))
        c.encryptionOverride = EncryptionMode(st.integer(8));
    c.blocked = Contacts::isBlocked(ctx, c.addr);
    return c;
}

constexpr char kSelect[] = "SELECT c.id, c.addr, c.name, c.origin, c.fingerprint, c.verified, c.prefer_encrypt, "
                           "c.last_seen, p.encryption_mode FROM contacts c "
                           "LEFT JOIN contact_policy p ON p.contact_id = c.id ";

} // namespace

qint64 Contacts::touch(MailContext &ctx, const QString &addr, const QString &name, ContactOrigin origin)
{
    const QString a = mime::normalizeAddr(addr);
    if (a.isEmpty() || !a.contains(u'@'))
        return 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    ctx.db.run("INSERT INTO contacts(addr, name, origin, last_seen) VALUES(?, ?, ?, ?) "
               "ON CONFLICT(addr) DO UPDATE SET "
               "origin = max(origin, excluded.origin), "
               "name = CASE WHEN contacts.name = '' THEN excluded.name ELSE contacts.name END, "
               "last_seen = excluded.last_seen",
               a, name.trimmed(), int(origin), now);
    return ctx.db.queryInt("SELECT id FROM contacts WHERE addr=?", a).value_or(0);
}

std::optional<ContactInfo> Contacts::get(MailContext &ctx, const QString &addr)
{
    Statement st(ctx.db, (QByteArray(kSelect) + "WHERE c.addr=?").constData());
    st.bind(1, mime::normalizeAddr(addr));
    if (!st.step())
        return std::nullopt;
    return read(ctx, st);
}

std::optional<ContactInfo> Contacts::byId(MailContext &ctx, qint64 id)
{
    Statement st(ctx.db, (QByteArray(kSelect) + "WHERE c.id=?").constData());
    st.bind(1, id);
    if (!st.step())
        return std::nullopt;
    return read(ctx, st);
}

QList<ContactInfo> Contacts::list(MailContext &ctx, const QString &filter, int limit)
{
    Statement st(ctx.db, (QByteArray(kSelect)
                          + "WHERE (?1 = '' OR c.addr LIKE ?2 OR c.name LIKE ?2) "
                            "ORDER BY c.origin >= 20 DESC, lower(CASE WHEN c.name = '' THEN c.addr ELSE c.name END) "
                            "LIMIT ?3")
                             .constData());
    st.bind(1, filter.trimmed());
    st.bind(2, QStringLiteral("%") + filter.trimmed() + u'%');
    st.bind(3, limit);
    QList<ContactInfo> out;
    while (st.step())
        out.append(read(ctx, st));
    return out;
}

bool Contacts::isTrusted(MailContext &ctx, const QString &addr)
{
    const QString a = mime::normalizeAddr(addr);
    if (a == mime::normalizeAddr(ctx.config.get(cfg::Addr)))
        return true;
    const auto c = get(ctx, a);
    return c && c->isKnown();
}

bool Contacts::adoptKey(MailContext &ctx, const QString &addr, const QString &fpr, bool preferEncrypt,
                        const QDateTime &seenAt, bool fromGossip)
{
    const auto c = get(ctx, addr);
    if (!c)
        return false;
    if (c->fingerprint == fpr) {
        if (!fromGossip)
            ctx.db.run("UPDATE contacts SET prefer_encrypt=?, key_seen_at=max(coalesce(key_seen_at, 0), ?) WHERE id=?",
                       preferEncrypt, seenAt.toMSecsSinceEpoch(), c->id);
        return false;
    }
    if (c->verified)
        return false; // a verified key is pinned until the user re-verifies
    if (!c->fingerprint.isEmpty()) {
        if (fromGossip)
            return false; // gossip never overrides a key the contact sent themselves
        const qint64 seen = ctx.db.queryInt("SELECT key_seen_at FROM contacts WHERE id=?", c->id).value_or(0);
        if (seenAt.toMSecsSinceEpoch() < seen)
            return false;
    }
    ctx.db.run("UPDATE contacts SET fingerprint=?, prefer_encrypt=?, key_seen_at=? WHERE id=?", fpr,
               fromGossip ? c->preferEncrypt : preferEncrypt, seenAt.toMSecsSinceEpoch(), c->id);
    ctx.ops.record(op::KeyLearned, {{QStringLiteral("addr"), c->addr}, {QStringLiteral("fpr"), fpr}});
    return true;
}

void Contacts::setEncryptionOverride(MailContext &ctx, qint64 contactId, std::optional<EncryptionMode> mode)
{
    if (mode)
        ctx.db.run("INSERT OR REPLACE INTO contact_policy(contact_id, encryption_mode) VALUES(?, ?)", contactId,
                   int(*mode));
    else
        ctx.db.run("DELETE FROM contact_policy WHERE contact_id=?", contactId);
}

void Contacts::setName(MailContext &ctx, qint64 contactId, const QString &name)
{
    ctx.db.run("UPDATE contacts SET name=?, origin=max(origin, ?) WHERE id=?", name.trimmed(),
               int(ContactOrigin::Manual), contactId);
    if (const auto c = byId(ctx, contactId))
        ctx.ops.record(op::ContactEdit, {{QStringLiteral("addr"), c->addr}, {QStringLiteral("name"), name}});
}

void Contacts::setVerified(MailContext &ctx, const QString &addr, const QString &fpr)
{
    touch(ctx, addr, QString(), ContactOrigin::Manual);
    ctx.db.run("UPDATE contacts SET fingerprint=?, verified=1 WHERE addr=?", fpr, mime::normalizeAddr(addr));
}

QString Contacts::normalizePattern(const QString &pattern)
{
    return pattern.trimmed().toLower();
}

void Contacts::block(MailContext &ctx, const QString &pattern)
{
    const QString norm = normalizePattern(pattern);
    if (norm.isEmpty() || !norm.contains(u'@'))
        return;
    ctx.db.run("INSERT OR IGNORE INTO blocklist(pattern_norm, pattern, created_at) VALUES(?, ?, ?)", norm,
               pattern.trimmed(), QDateTime::currentMSecsSinceEpoch());
    ctx.ops.record(op::Block, {{QStringLiteral("pattern"), norm}});
}

void Contacts::unblock(MailContext &ctx, const QString &pattern)
{
    const QString norm = normalizePattern(pattern);
    ctx.db.run("DELETE FROM blocklist WHERE pattern_norm=?", norm);
    ctx.ops.record(op::Unblock, {{QStringLiteral("pattern"), norm}});
}

QStringList Contacts::blocklist(MailContext &ctx)
{
    QStringList out;
    Statement st(ctx.db, "SELECT pattern FROM blocklist ORDER BY pattern_norm");
    while (st.step())
        out.append(st.text(0));
    return out;
}

bool Contacts::isBlocked(MailContext &ctx, const QString &addr)
{
    const QString a = mime::normalizeAddr(addr);
    const QString domain = a.contains(u'@') ? a.mid(a.indexOf(u'@')) : QString();
    return ctx.db.queryInt("SELECT 1 FROM blocklist WHERE pattern_norm IN (?, ?)", a, domain).has_value();
}

} // namespace e3::mail
