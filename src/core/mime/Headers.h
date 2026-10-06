// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

namespace e3::mime {

struct Address
{
    QString name;
    QString addr;

    bool operator==(const Address &o) const = default;
    // "Name <addr>" for display, or just the address.
    QString display() const;
};

// Lower-cases the whole address. Local parts are case-sensitive in theory and
// never in practice; treating them so is what makes one person one contact.
QString normalizeAddr(const QString &addr);

// RFC 5322 address-list, tolerant of what real mail contains: quoted and
// encoded display names, comments, groups, bare addresses.
QList<Address> parseAddressList(const QByteArray &raw);
QByteArray formatAddress(const Address &a);
QByteArray formatAddressList(const QList<Address> &list);

// RFC 5322 date, including obsolete zone names. Invalid on failure.
QDateTime parseDate(const QByteArray &raw);
QByteArray formatDate(const QDateTime &dt);

// "<a@b> <c@d>" -> {"a@b", "c@d"}. Tolerates missing brackets.
QStringList parseMessageIds(const QByteArray &raw);
QString generateMessageId(const QString &fromAddr);

// Ordered header list as it appeared, with folding undone.
class HeaderList
{
public:
    struct Field
    {
        QByteArray name;
        QByteArray value; // raw bytes, unfolded, leading whitespace trimmed
    };

    static HeaderList parse(const QByteArray &block);

    QByteArray raw(const QByteArray &name) const;       // first occurrence
    QList<QByteArray> all(const QByteArray &name) const;
    QString text(const QByteArray &name) const;         // RFC 2047 decoded
    bool has(const QByteArray &name) const;
    void append(const QByteArray &name, const QByteArray &value);
    void set(const QByteArray &name, const QByteArray &value);
    void remove(const QByteArray &name);
    const QList<Field> &fields() const { return m_fields; }

private:
    QList<Field> m_fields;
};

} // namespace e3::mime
