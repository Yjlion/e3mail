// SPDX-License-Identifier: MPL-2.0
#include "VCard.h"

#include "mime/Encoding.h"
#include "mime/Headers.h"

#include <QDate>
#include <QRegularExpression>

namespace e3::mail {

namespace {

struct Property
{
    QByteArray name;                  // upper case, without group
    QList<QByteArray> types;          // lower case: TYPE=a,b and 2.1's bare ;A;B
    QByteArray encoding, charset;     // 2.1
    QByteArray value;                 // raw, still escaped
};

// Physical lines into logical ones: a line starting with a space or tab
// continues the one before (RFC 6350 §3.2), and in 2.1 a quoted-printable
// value ending in '=' continues on the next line.
QList<QByteArray> unfold(const QByteArray &data)
{
    QList<QByteArray> out;
    for (QByteArray line : data.split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        if (!out.isEmpty() && (line.startsWith(' ') || line.startsWith('\t'))) {
            out.last() += line.mid(1);
            continue;
        }
        if (!out.isEmpty() && out.last().endsWith('=') && out.last().toUpper().contains("QUOTED-PRINTABLE")) {
            out.last().chop(1);
            out.last() += line;
            continue;
        }
        out.append(line);
    }
    return out;
}

std::optional<Property> parseLine(const QByteArray &line)
{
    // The first ':' outside a quoted parameter value ends the name part.
    qsizetype colon = -1;
    bool quoted = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        if (line[i] == '"')
            quoted = !quoted;
        else if (line[i] == ':' && !quoted) {
            colon = i;
            break;
        }
    }
    if (colon <= 0)
        return std::nullopt;
    Property p;
    p.value = line.mid(colon + 1);
    const QList<QByteArray> parts = line.left(colon).split(';');
    p.name = parts.first().toUpper();
    if (const qsizetype dot = p.name.lastIndexOf('.'); dot >= 0)
        p.name = p.name.mid(dot + 1); // "item1.EMAIL"
    for (qsizetype i = 1; i < parts.size(); ++i) {
        const QByteArray param = parts[i].trimmed();
        const qsizetype eq = param.indexOf('=');
        const QByteArray key = (eq < 0 ? QByteArray("TYPE") : param.left(eq)).toUpper();
        QByteArray val = eq < 0 ? param : param.mid(eq + 1);
        if (val.startsWith('"') && val.endsWith('"') && val.size() >= 2)
            val = val.mid(1, val.size() - 2);
        if (key == "TYPE") {
            for (const QByteArray &t : val.split(','))
                p.types.append(t.trimmed().toLower());
        } else if (key == "ENCODING") {
            p.encoding = val.toUpper();
        } else if (key == "CHARSET") {
            p.charset = val;
        }
    }
    // 2.1 had QUOTED-PRINTABLE as a bare parameter too.
    if (p.types.contains("quoted-printable"))
        p.encoding = "QUOTED-PRINTABLE";
    return p;
}

QString decodeValue(const Property &p)
{
    QByteArray bytes = p.value;
    if (p.encoding == "QUOTED-PRINTABLE")
        bytes = mime::decodeQuotedPrintable(bytes);
    else if (p.encoding == "B" || p.encoding == "BASE64")
        bytes = QByteArray::fromBase64(bytes);
    return p.charset.isEmpty() ? QString::fromUtf8(bytes) : mime::decodeCharset(bytes, p.charset);
}

// Splits on unescaped `sep` and unescapes each piece.
QStringList components(const QString &value, QChar sep)
{
    QStringList out;
    QString cur;
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar c = value[i];
        if (c == u'\\' && i + 1 < value.size()) {
            const QChar n = value[++i];
            cur += (n == u'n' || n == u'N') ? QChar(u'\n') : n;
        } else if (c == sep) {
            out.append(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.append(cur);
    return out;
}

QString unescape(const QString &value)
{
    // A text value is one component; a stray separator is kept as it is.
    return components(value, QChar(0)).join(QString());
}

QString phoneLabel(const QList<QByteArray> &types)
{
    if (types.contains("cell") || types.contains("mobile"))
        return QStringLiteral("mobile");
    if (types.contains("work"))
        return QStringLiteral("work");
    if (types.contains("home"))
        return QStringLiteral("home");
    return QStringLiteral("other");
}

QByteArray escape(const QString &text)
{
    QString s = text;
    s.replace(u'\\', QStringLiteral("\\\\"));
    s.replace(u',', QStringLiteral("\\,"));
    s.replace(u';', QStringLiteral("\\;"));
    s.replace(QStringLiteral("\r\n"), QStringLiteral("\\n"));
    s.replace(u'\n', QStringLiteral("\\n"));
    return s.toUtf8();
}

// Folds at 75 octets without splitting a UTF-8 sequence (RFC 6350 §3.2).
QByteArray fold(const QByteArray &line)
{
    QByteArray out;
    qsizetype start = 0;
    int limit = 75;
    while (line.size() - start > limit) {
        qsizetype end = start + limit;
        while (end > start && (uchar(line[end]) & 0xC0) == 0x80)
            --end; // back to the start of the character
        out += line.mid(start, end - start) + "\r\n ";
        start = end;
        limit = 74; // the leading space counts
    }
    out += line.mid(start) + "\r\n";
    return out;
}

QByteArray birthdayOut(const QString &iso)
{
    if (iso.startsWith(QLatin1String("--")))
        return "--" + iso.mid(2).remove(u'-').toLatin1();
    return QString(iso).remove(u'-').toLatin1();
}

} // namespace

QString VCard::normalizeBirthday(const QString &value)
{
    QString v = value.trimmed();
    if (const qsizetype t = v.indexOf(u'T'); t > 0)
        v = v.left(t); // a time part
    static const QRegularExpression full(QStringLiteral("^(\\d{4})-?(\\d{2})-?(\\d{2})$"));
    static const QRegularExpression noYear(QStringLiteral("^--(\\d{2})-?(\\d{2})$"));
    if (const auto m = full.match(v); m.hasMatch()) {
        const QDate d(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
        return d.isValid() ? d.toString(Qt::ISODate) : QString();
    }
    if (const auto m = noYear.match(v); m.hasMatch()) {
        // 2000 is a leap year, so 29 February is accepted.
        const QDate d(2000, m.captured(1).toInt(), m.captured(2).toInt());
        return d.isValid() ? QStringLiteral("--%1-%2").arg(m.captured(1), m.captured(2)) : QString();
    }
    return {};
}

QList<VCard::Card> VCard::parse(const QByteArray &data)
{
    QList<Card> out;
    std::optional<Card> card;
    QString formatted, structured;
    int depth = 0;
    for (const QByteArray &line : unfold(data)) {
        const auto p = parseLine(line);
        if (!p)
            continue;
        const QString value = decodeValue(*p);
        if (p->name == "BEGIN" && value.trimmed().compare(QLatin1String("VCARD"), Qt::CaseInsensitive) == 0) {
            if (depth++ == 0) {
                card = Card();
                formatted.clear();
                structured.clear();
            }
            continue;
        }
        if (p->name == "END" && value.trimmed().compare(QLatin1String("VCARD"), Qt::CaseInsensitive) == 0) {
            if (depth > 0 && --depth == 0 && card) {
                card->name = !formatted.isEmpty() ? formatted : structured;
                out.append(*card);
                card.reset();
            }
            continue;
        }
        if (!card || depth != 1)
            continue; // outside a card, or inside an embedded one (2.1 AGENT)
        if (p->name == "FN") {
            formatted = unescape(value).trimmed();
        } else if (p->name == "N") {
            // Family; Given; Additional; Prefix; Suffix
            const QStringList n = components(value, u';');
            QStringList parts;
            for (int i : {3, 1, 2, 0, 4}) {
                if (n.value(i).trimmed().size())
                    parts.append(n.value(i).trimmed());
            }
            structured = parts.join(u' ');
        } else if (p->name == "EMAIL") {
            QString addr = unescape(value).trimmed();
            if (addr.startsWith(QLatin1String("mailto:"), Qt::CaseInsensitive))
                addr = addr.mid(7);
            addr = mime::normalizeAddr(addr);
            if (addr.contains(u'@') && !card->emails.contains(addr))
                card->emails.append(addr);
        } else if (p->name == "ORG") {
            QStringList org = components(value, u';');
            org.removeAll(QString());
            card->organization = org.join(QStringLiteral(", ")).trimmed();
        } else if (p->name == "TITLE") {
            card->title = unescape(value).trimmed();
        } else if (p->name == "NOTE") {
            const QString note = unescape(value).trimmed();
            card->notes = card->notes.isEmpty() ? note : card->notes + u'\n' + note;
        } else if (p->name == "BDAY") {
            card->birthday = normalizeBirthday(unescape(value));
        } else if (p->name == "TEL") {
            QString number = unescape(value).trimmed();
            if (number.startsWith(QLatin1String("tel:"), Qt::CaseInsensitive))
                number = number.mid(4);
            if (!number.isEmpty())
                card->phones.append({phoneLabel(p->types), number});
        }
    }
    return out;
}

QByteArray VCard::emit(const QList<ContactInfo> &contacts)
{
    QByteArray out;
    for (const ContactInfo &c : contacts) {
        out += "BEGIN:VCARD\r\nVERSION:4.0\r\n";
        out += fold("FN:" + escape(c.name.isEmpty() ? c.addr : c.name));
        out += fold("EMAIL:" + escape(c.addr));
        if (!c.organization.isEmpty())
            out += fold("ORG:" + escape(c.organization));
        if (!c.title.isEmpty())
            out += fold("TITLE:" + escape(c.title));
        for (const ContactPhone &p : c.phones) {
            const QByteArray type = p.label == QLatin1String("mobile") ? "cell"
                : p.label == QLatin1String("work")                     ? "work"
                : p.label == QLatin1String("home")                     ? "home"
                                                                       : "voice";
            out += fold("TEL;TYPE=" + type + ':' + escape(p.number));
        }
        if (!c.birthday.isEmpty())
            out += fold("BDAY:" + birthdayOut(c.birthday));
        if (!c.notes.isEmpty())
            out += fold("NOTE:" + escape(c.notes));
        out += "END:VCARD\r\n";
    }
    return out;
}

} // namespace e3::mail
