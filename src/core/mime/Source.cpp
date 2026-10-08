// SPDX-License-Identifier: MPL-2.0
#include "Source.h"

#include "Part.h"

#include <QCoreApplication>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>

namespace e3::mime {

namespace {

void walk(const Part &p, int depth, QList<Source::Node> &out)
{
    Source::Node n;
    n.depth = depth;
    n.mimeType = p.mimeType;
    n.encoding = p.headers.raw("Content-Transfer-Encoding").trimmed().toLower();
    n.filename = p.filename;
    n.size = p.isMultipart() ? p.raw.size() : p.body.size();
    out.append(n);
    for (const Part &c : p.children)
        walk(c, depth + 1, out);
}

// Header fields as they appear, folded lines kept with their field.
struct RawField
{
    QByteArray name; // lower case
    QByteArray text; // the field's lines, each ending in CRLF
};

QList<RawField> rawFields(const QByteArray &block)
{
    QList<RawField> out;
    for (QByteArray line : block.split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.isEmpty())
            continue;
        if ((line.startsWith(' ') || line.startsWith('\t')) && !out.isEmpty()) {
            out.last().text += line + "\r\n";
            continue;
        }
        out.append({line.left(qMax<qsizetype>(0, line.indexOf(':'))).trimmed().toLower(), line + "\r\n"});
    }
    return out;
}

} // namespace

QList<Source::Node> Source::outline(const QByteArray &raw)
{
    QList<Node> out;
    walk(parse(raw), 0, out);
    return out;
}

QString Source::shorten(const QString &source)
{
    // Indented too: a folded header such as Autocrypt's keydata.
    static const QRegularExpression base64Line(QStringLiteral("^([ \\t]*)([A-Za-z0-9+/]{40,}={0,2})$"));
    const QStringList lines = source.split(u'\n');
    QStringList out;
    out.reserve(lines.size());
    for (qsizetype i = 0; i < lines.size();) {
        qsizetype j = i;
        qint64 chars = 0;
        QString indent;
        while (j < lines.size()) {
            QString l = lines[j];
            if (l.endsWith(u'\r'))
                l.chop(1);
            const auto m = base64Line.match(l);
            if (!m.hasMatch() || (j > i && m.captured(1) != indent))
                break;
            indent = m.captured(1);
            chars += m.capturedLength(2);
            ++j;
        }
        if (j - i > 4) {
            // Four characters of base64 carry three bytes. The marker keeps
            // the indentation, so a folded header stays one header, and is
            // a bidi isolate (FSI…PDI), so a right-to-left translation keeps
            // its brackets in order inside the left-to-right source.
            out.append(indent + QChar(0x2068)
                       + QCoreApplication::translate("Source", "[… %1 of encoded data …]")
                             .arg(QLocale().formattedDataSize(chars * 3 / 4))
                       + QChar(0x2069));
            i = j;
        } else {
            const qsizetype end = qMax(j, i + 1);
            for (; i < end; ++i)
                out.append(lines[i]);
        }
    }
    return out.join(u'\n');
}

QByteArray Source::decryptedEml(const QByteArray &outer, const QByteArray &inner)
{
    QByteArray outerHeaders, outerBody, innerHeaders, innerBody;
    splitHeaderBody(outer, &outerHeaders, &outerBody);
    splitHeaderBody(inner, &innerHeaders, &innerBody);
    const QList<RawField> innerFields = rawFields(innerHeaders);
    QSet<QByteArray> innerNames;
    for (const RawField &f : innerFields)
        innerNames.insert(f.name);

    QByteArray out;
    for (const RawField &f : rawFields(outerHeaders)) {
        if (f.name.startsWith("content-") || f.name == "mime-version" || innerNames.contains(f.name))
            continue;
        out += f.text;
    }
    out += "MIME-Version: 1.0\r\n";
    for (const RawField &f : innerFields) {
        if (f.name != "mime-version")
            out += f.text;
    }
    out += "\r\n";
    out += innerBody;
    return out;
}

} // namespace e3::mime
