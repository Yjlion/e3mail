// SPDX-License-Identifier: MPL-2.0
#include "Contacts.h"

#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"

#include <QJsonArray>
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
    c.organization = st.text(9);
    c.title = st.text(10);
    c.notes = st.text(11);
    c.birthday = st.text(12);
    c.blocked = Contacts::isBlocked(ctx, c.addr);
    Statement ph(ctx.db, "SELECT label, number FROM contact_phones WHERE contact_id=? ORDER BY idx");
    ph.bind(1, c.id);
    while (ph.step())
        c.phones.append({ph.text(0), ph.text(1)});
    return c;
}

constexpr char kSelect[] = "SELECT c.id, c.addr, c.name, c.origin, c.fingerprint, c.verified, c.prefer_encrypt, "
                           "c.last_seen, p.encryption_mode, c.organization, c.title, c.notes, c.birthday "
                           "FROM contacts c LEFT JOIN contact_policy p ON p.contact_id = c.id ";

QJsonObject detailsPayload(const QString &addr, const ContactInfo &d)
{
    QJsonArray phones;
    for (const ContactPhone &p : d.phones)
        phones.append(QJsonObject{{QStringLiteral("label"), p.label}, {QStringLiteral("number"), p.number}});
    return {{QStringLiteral("addr"), addr},
            {QStringLiteral("organization"), d.organization},
            {QStringLiteral("title"), d.title},
            {QStringLiteral("notes"), d.notes},
            {QStringLiteral("birthday"), d.birthday},
            {QStringLiteral("phones"), phones}};
}

ContactInfo cleaned(const ContactInfo &d)
{
    ContactInfo c = d;
    c.name = d.name.trimmed();
    c.organization = d.organization.trimmed();
    c.title = d.title.trimmed();
    c.notes = d.notes.trimmed();
    c.birthday = VCard::normalizeBirthday(d.birthday);
    c.phones.clear();
    for (const ContactPhone &p : d.phones) {
        if (!p.number.trimmed().isEmpty())
            c.phones.append({p.label.isEmpty() ? QStringLiteral("other") : p.label, p.number.trimmed()});
    }
    return c;
}

// Without a transaction or an op: shared by the public calls and OpApply.
void writeDetails(MailContext &ctx, qint64 id, const ContactInfo &d)
{
    ctx.db.run("UPDATE contacts SET organization=?, title=?, notes=?, birthday=? WHERE id=?", d.organization, d.title,
               d.notes, d.birthday, id);
    ctx.db.run("DELETE FROM contact_phones WHERE contact_id=?", id);
    for (qsizetype i = 0; i < d.phones.size(); ++i)
        ctx.db.run("INSERT INTO contact_phones(contact_id, idx, label, number) VALUES(?, ?, ?, ?)", id, int(i),
                   d.phones[i].label, d.phones[i].number);
}

void recordName(MailContext &ctx, const QString &addr, const QString &name)
{
    ctx.ops.record(op::ContactEdit, {{QStringLiteral("addr"), addr}, {QStringLiteral("name"), name}});
}

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
    if (const auto c = byId(ctx, contactId))
        ctx.ops.record(op::ContactPolicy, {{QStringLiteral("addr"), c->addr},
                                           {QStringLiteral("mode"), mode ? QJsonValue(int(*mode)) : QJsonValue()}});
}

void Contacts::setName(MailContext &ctx, qint64 contactId, const QString &name)
{
    ctx.db.run("UPDATE contacts SET name=?, origin=max(origin, ?) WHERE id=?", name.trimmed(),
               int(ContactOrigin::Manual), contactId);
    if (const auto c = byId(ctx, contactId))
        ctx.ops.record(op::ContactEdit, {{QStringLiteral("addr"), c->addr}, {QStringLiteral("name"), name}});
}

std::optional<qint64> Contacts::create(MailContext &ctx, const QString &addr, const QString &name)
{
    const QString a = mime::normalizeAddr(addr);
    if (!a.contains(u'@') || a.startsWith(u'@') || a.endsWith(u'@') || a.contains(u' '))
        return std::nullopt;
    if (const auto c = get(ctx, a); c && int(c->origin) >= int(ContactOrigin::Manual))
        return std::nullopt;
    Transaction tx(ctx.db);
    const qint64 id = touch(ctx, a, QString(), ContactOrigin::Manual);
    if (!name.trimmed().isEmpty())
        ctx.db.run("UPDATE contacts SET name=? WHERE id=?", name.trimmed(), id);
    recordName(ctx, a, name.trimmed().isEmpty() ? get(ctx, a)->name : name.trimmed());
    tx.commit();
    return id;
}

void Contacts::setDetails(MailContext &ctx, qint64 contactId, const ContactInfo &details)
{
    const auto c = byId(ctx, contactId);
    if (!c)
        return;
    const ContactInfo d = cleaned(details);
    Transaction tx(ctx.db);
    if (d.name != c->name) {
        ctx.db.run("UPDATE contacts SET name=?, origin=max(origin, ?) WHERE id=?", d.name, int(ContactOrigin::Manual),
                   contactId);
        recordName(ctx, c->addr, d.name);
    }
    if (d.organization != c->organization || d.title != c->title || d.notes != c->notes || d.birthday != c->birthday
        || d.phones != c->phones) {
        writeDetails(ctx, contactId, d);
        ctx.db.run("UPDATE contacts SET origin=max(origin, ?) WHERE id=?", int(ContactOrigin::Manual), contactId);
        ctx.ops.record(op::ContactDetails, detailsPayload(c->addr, d));
    }
    tx.commit();
}

void Contacts::remove(MailContext &ctx, qint64 contactId)
{
    const auto c = byId(ctx, contactId);
    if (!c)
        return;
    Transaction tx(ctx.db);
    ctx.db.run("DELETE FROM contacts WHERE id=?", contactId); // phones and policy cascade
    ctx.ops.record(op::ContactRemove, {{QStringLiteral("addr"), c->addr}});
    tx.commit();
}

Contacts::ImportResult Contacts::import(MailContext &ctx, const QList<VCard::Card> &cards)
{
    ImportResult r;
    Transaction tx(ctx.db);
    for (const VCard::Card &card : cards) {
        if (card.emails.isEmpty()) {
            ++r.skipped;
            continue;
        }
        ContactInfo in;
        in.name = card.name;
        in.organization = card.organization;
        in.title = card.title;
        in.notes = card.notes;
        in.birthday = card.birthday;
        in.phones = card.phones;
        in = cleaned(in);
        for (const QString &addr : card.emails) {
            const auto existing = get(ctx, addr);
            const qint64 id = touch(ctx, addr, QString(), ContactOrigin::Manual);
            if (!id)
                continue;
            const ContactInfo have = existing ? *existing : ContactInfo();
            // Only what is missing: nothing here is overwritten.
            ContactInfo d = have;
            bool changed = !existing || int(existing->origin) < int(ContactOrigin::Manual);
            auto fill = [&changed](QString &field, const QString &value) {
                if (field.isEmpty() && !value.isEmpty()) {
                    field = value;
                    changed = true;
                }
            };
            fill(d.organization, in.organization);
            fill(d.title, in.title);
            fill(d.notes, in.notes);
            fill(d.birthday, in.birthday);
            if (d.phones.isEmpty() && !in.phones.isEmpty()) {
                d.phones = in.phones;
                changed = true;
            }
            if (have.name.isEmpty() && !in.name.isEmpty()) {
                ctx.db.run("UPDATE contacts SET name=? WHERE id=?", in.name, id);
                recordName(ctx, mime::normalizeAddr(addr), in.name);
                changed = true;
            } else if (!existing || int(existing->origin) < int(ContactOrigin::Manual)) {
                recordName(ctx, mime::normalizeAddr(addr), have.name); // into the book, on every device
            }
            if (d.organization != have.organization || d.title != have.title || d.notes != have.notes
                || d.birthday != have.birthday || d.phones != have.phones) {
                writeDetails(ctx, id, d);
                ctx.ops.record(op::ContactDetails, detailsPayload(mime::normalizeAddr(addr), d));
            }
            if (!existing)
                ++r.added;
            else if (changed)
                ++r.updated;
        }
    }
    tx.commit();
    return r;
}

void Contacts::applyDetails(MailContext &ctx, const QString &addr, const QJsonObject &payload)
{
    const qint64 id = touch(ctx, addr, QString(), ContactOrigin::Manual);
    if (!id)
        return;
    ContactInfo d;
    d.organization = payload.value(QLatin1String("organization")).toString();
    d.title = payload.value(QLatin1String("title")).toString();
    d.notes = payload.value(QLatin1String("notes")).toString();
    d.birthday = payload.value(QLatin1String("birthday")).toString();
    for (const QJsonValue &v : payload.value(QLatin1String("phones")).toArray()) {
        const QJsonObject o = v.toObject();
        d.phones.append({o.value(QLatin1String("label")).toString(), o.value(QLatin1String("number")).toString()});
    }
    writeDetails(ctx, id, cleaned(d));
}

void Contacts::setVerified(MailContext &ctx, const QString &addr, const QString &fpr)
{
    touch(ctx, addr, QString(), ContactOrigin::Manual);
    ctx.db.run("UPDATE contacts SET fingerprint=?, verified=1 WHERE addr=?", fpr, mime::normalizeAddr(addr));
    ctx.ops.record(op::ContactVerify, {{QStringLiteral("addr"), mime::normalizeAddr(addr)}, {QStringLiteral("fpr"), fpr}});
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
