// SPDX-License-Identifier: MPL-2.0
#include "Headers.h"

#include "Encoding.h"

#include <QRandomGenerator>
#include <QTimeZone>
#include <QUuid>

#include <cstring>

namespace e3::mime {

QString Address::display() const
{
    if (name.isEmpty() || name == addr)
        return addr;
    return name + QStringLiteral(" <") + addr + u'>';
}

QString normalizeAddr(const QString &addr)
{
    return addr.trimmed().toLower();
}

namespace {

// Strips (comments), honouring nesting and quoting.
QByteArray stripComments(const QByteArray &in, QByteArray *comment = nullptr)
{
    QByteArray out;
    int depth = 0;
    bool quoted = false;
    for (qsizetype i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '\\' && i + 1 < in.size()) {
            if (depth == 0) {
                out.append(c);
                out.append(in[i + 1]);
            } else if (comment) {
                comment->append(in[i + 1]);
            }
            ++i;
            continue;
        }
        if (!depth && c == '"')
            quoted = !quoted;
        if (!quoted && c == '(') {
            ++depth;
            continue;
        }
        if (!quoted && c == ')' && depth > 0) {
            --depth;
            continue;
        }
        if (depth == 0)
            out.append(c);
        else if (comment)
            comment->append(c);
    }
    return out;
}

QString unquoteName(QByteArray name)
{
    name = name.trimmed();
    QByteArray out;
    bool quoted = false;
    for (qsizetype i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '\\' && i + 1 < name.size()) {
            out.append(name[++i]);
        } else if (c == '"') {
            quoted = !quoted;
        } else {
            out.append(c);
        }
    }
    return decodeHeader(out.simplified()).trimmed();
}

Address parseMailbox(const QByteArray &raw)
{
    Address a;
    const qsizetype lt = raw.lastIndexOf('<');
    const qsizetype gt = raw.lastIndexOf('>');
    if (lt >= 0 && gt > lt) {
        a.addr = QString::fromUtf8(stripComments(raw.mid(lt + 1, gt - lt - 1))).trimmed();
        a.name = unquoteName(stripComments(raw.left(lt)));
        if (a.name.isEmpty()) {
            QByteArray c;
            stripComments(raw.mid(gt + 1), &c);
            a.name = decodeHeader(c.trimmed());
        }
    } else {
        QByteArray comment;
        a.addr = QString::fromUtf8(stripComments(raw, &comment)).trimmed();
        a.name = decodeHeader(comment.trimmed());
    }
    if (a.addr.startsWith(u'"') || a.addr.contains(u' '))
        a.addr = a.addr.simplified().remove(u' ').remove(u'"');
    return a;
}

} // namespace

QList<Address> parseAddressList(const QByteArray &raw)
{
    QList<Address> out;
    QByteArray cur;
    bool quoted = false;
    int angle = 0;
    int paren = 0;
    auto flush = [&] {
        const QByteArray t = cur.trimmed();
        cur.clear();
        if (t.isEmpty())
            return;
        const Address a = parseMailbox(t);
        if (a.addr.contains(u'@'))
            out.append(a);
    };
    for (qsizetype i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '\\' && i + 1 < raw.size()) {
            cur.append(c);
            cur.append(raw[++i]);
            continue;
        }
        if (!paren && c == '"')
            quoted = !quoted;
        else if (!quoted && c == '(')
            ++paren;
        else if (!quoted && c == ')' && paren)
            --paren;
        else if (!quoted && !paren && c == '<')
            ++angle;
        else if (!quoted && !paren && c == '>' && angle)
            --angle;
        if (!quoted && !paren && !angle) {
            if (c == ',' || c == ';') {
                flush();
                continue;
            }
            if (c == ':') {
                cur.clear(); // group name: "Friends: a@b, c@d;"
                continue;
            }
        }
        cur.append(c);
    }
    flush();
    return out;
}

QByteArray formatAddress(const Address &a)
{
    if (a.name.isEmpty())
        return a.addr.toUtf8();
    QByteArray name;
    const QByteArray utf8 = a.name.toUtf8();
    if (!isAscii(utf8)) {
        name = encodeHeader(a.name);
    } else {
        bool special = false;
        for (char c : utf8) {
            if (std::strchr("()<>[]:;@\\,.\"", c))
                special = true;
        }
        if (special) {
            QByteArray q = utf8;
            q.replace('\\', "\\\\").replace('"', "\\\"");
            name = '"' + q + '"';
        } else {
            name = utf8;
        }
    }
    return name + " <" + a.addr.toUtf8() + '>';
}

QByteArray formatAddressList(const QList<Address> &list)
{
    QByteArray out;
    for (const Address &a : list) {
        if (!out.isEmpty())
            out.append(", ");
        out.append(formatAddress(a));
    }
    return out;
}

QDateTime parseDate(const QByteArray &raw)
{
    QByteArray s = stripComments(raw).simplified();
    // Drop a leading day name.
    if (const qsizetype comma = s.indexOf(','); comma >= 0 && comma < 5)
        s = s.mid(comma + 1).trimmed();
    QList<QByteArray> tok = s.split(' ');
    if (tok.size() < 4)
        return {};
    static const char *months[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                   "jul", "aug", "sep", "oct", "nov", "dec"};
    bool ok = false;
    const int day = tok[0].toInt(&ok);
    if (!ok)
        return {};
    int month = 0;
    const QByteArray mon = tok[1].left(3).toLower();
    for (int i = 0; i < 12; ++i) {
        if (mon == months[i])
            month = i + 1;
    }
    if (!month)
        return {};
    int year = tok[2].toInt(&ok);
    if (!ok)
        return {};
    if (tok[2].size() <= 2)
        year += year < 50 ? 2000 : 1900;
    const QList<QByteArray> hms = tok[3].split(':');
    if (hms.size() < 2)
        return {};
    const int h = hms[0].toInt();
    const int m = hms[1].toInt();
    const int sec = hms.size() > 2 ? hms[2].toInt() : 0;
    int offsetSecs = 0;
    if (tok.size() > 4) {
        const QByteArray z = tok[4].toUpper();
        if ((z.startsWith('+') || z.startsWith('-')) && z.size() >= 5) {
            const int hh = z.mid(1, 2).toInt();
            const int mm = z.mid(3, 2).toInt();
            offsetSecs = (hh * 3600 + mm * 60) * (z.startsWith('-') ? -1 : 1);
        } else {
            static const struct
            {
                const char *name;
                int hours;
            } zones[] = {{"UT", 0},   {"GMT", 0},  {"Z", 0},    {"EST", -5}, {"EDT", -4},
                         {"CST", -6}, {"CDT", -5}, {"MST", -7}, {"MDT", -6}, {"PST", -8},
                         {"PDT", -7}};
            for (const auto &zone : zones) {
                if (z == zone.name)
                    offsetSecs = zone.hours * 3600;
            }
        }
    }
    const QDate date(year, month, day);
    const QTime time(h, m, sec);
    if (!date.isValid() || !time.isValid())
        return {};
    return QDateTime(date, time, QTimeZone::fromSecondsAheadOfUtc(offsetSecs)).toUTC();
}

QByteArray formatDate(const QDateTime &dt)
{
    static const char *days[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const QDateTime u = dt.toUTC();
    return QByteArray(days[u.date().dayOfWeek() - 1]) + ", " + QByteArray::number(u.date().day()) + ' '
        + months[u.date().month() - 1] + ' ' + QByteArray::number(u.date().year()) + ' '
        + u.time().toString(QStringLiteral("HH:mm:ss")).toLatin1() + " +0000";
}

QStringList parseMessageIds(const QByteArray &raw)
{
    QStringList ids;
    const QByteArray s = stripComments(raw);
    qsizetype i = 0;
    bool sawBracket = false;
    while (true) {
        const qsizetype lt = s.indexOf('<', i);
        if (lt < 0)
            break;
        const qsizetype gt = s.indexOf('>', lt);
        if (gt < 0)
            break;
        sawBracket = true;
        const QString id = QString::fromUtf8(s.mid(lt + 1, gt - lt - 1)).trimmed();
        if (!id.isEmpty() && !ids.contains(id))
            ids.append(id);
        i = gt + 1;
    }
    if (!sawBracket) {
        for (const QByteArray &t : s.simplified().split(' ')) {
            if (t.contains('@'))
                ids.append(QString::fromUtf8(t));
        }
    }
    return ids;
}

QString generateMessageId(const QString &fromAddr)
{
    QString domain = fromAddr.section(u'@', 1);
    if (domain.isEmpty())
        domain = QStringLiteral("localhost");
    return QUuid::createUuid().toString(QUuid::WithoutBraces) + u'@' + domain;
}

HeaderList HeaderList::parse(const QByteArray &block)
{
    HeaderList h;
    const QList<QByteArray> lines = block.split('\n');
    for (QByteArray line : lines) {
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.isEmpty())
            continue;
        if ((line[0] == ' ' || line[0] == '\t') && !h.m_fields.isEmpty()) {
            // RFC 5322 unfolding: the CRLF goes, the whitespace stays.
            h.m_fields.last().value.append(line);
            continue;
        }
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        h.m_fields.append({line.left(colon).trimmed(), line.mid(colon + 1).trimmed()});
    }
    for (Field &f : h.m_fields)
        f.value = f.value.trimmed();
    return h;
}

QByteArray HeaderList::raw(const QByteArray &name) const
{
    for (const Field &f : m_fields) {
        if (f.name.compare(name, Qt::CaseInsensitive) == 0)
            return f.value;
    }
    return {};
}

QList<QByteArray> HeaderList::all(const QByteArray &name) const
{
    QList<QByteArray> out;
    for (const Field &f : m_fields) {
        if (f.name.compare(name, Qt::CaseInsensitive) == 0)
            out.append(f.value);
    }
    return out;
}

QString HeaderList::text(const QByteArray &name) const
{
    return decodeHeader(raw(name)).simplified();
}

bool HeaderList::has(const QByteArray &name) const
{
    for (const Field &f : m_fields) {
        if (f.name.compare(name, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

void HeaderList::append(const QByteArray &name, const QByteArray &value)
{
    m_fields.append({name, value});
}

void HeaderList::set(const QByteArray &name, const QByteArray &value)
{
    remove(name);
    append(name, value);
}

void HeaderList::remove(const QByteArray &name)
{
    m_fields.removeIf([&](const Field &f) { return f.name.compare(name, Qt::CaseInsensitive) == 0; });
}

} // namespace e3::mime
