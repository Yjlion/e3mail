// SPDX-License-Identifier: MPL-2.0
#include "Part.h"

#include "Encoding.h"

namespace e3::mime {

namespace {

constexpr int kMaxDepth = 32;

Part parseEntity(const QByteArray &raw, int depth, const QByteArray &defaultType);

QList<QByteArray> splitMultipart(const QByteArray &body, const QByteArray &boundary)
{
    // RFC 2046: a delimiter is CRLF "--" boundary at the start of a line. The
    // CRLF before it belongs to the delimiter, not to the preceding part.
    QList<QByteArray> parts;
    const QByteArray delim = "--" + boundary;
    qsizetype pos = 0;
    qsizetype partStart = -1;
    while (pos <= body.size()) {
        qsizetype lineEnd = body.indexOf('\n', pos);
        if (lineEnd < 0)
            lineEnd = body.size();
        QByteArray line = body.mid(pos, lineEnd - pos);
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.startsWith(delim)) {
            const QByteArray rest = line.mid(delim.size()).trimmed();
            if (rest.isEmpty() || rest == "--") {
                if (partStart >= 0) {
                    qsizetype end = pos;
                    if (end > partStart && body[end - 1] == '\n')
                        --end;
                    if (end > partStart && body[end - 1] == '\r')
                        --end;
                    parts.append(body.mid(partStart, end - partStart));
                }
                if (rest == "--")
                    return parts;
                partStart = lineEnd + 1;
            }
        }
        pos = lineEnd + 1;
    }
    // Missing close delimiter: keep what we have.
    if (partStart >= 0 && partStart < body.size())
        parts.append(body.mid(partStart));
    return parts;
}

Part parseEntity(const QByteArray &raw, int depth, const QByteArray &defaultType)
{
    Part p;
    p.raw = raw;
    QByteArray headerBlock;
    QByteArray body;
    splitHeaderBody(raw, &headerBlock, &body);
    p.headers = HeaderList::parse(headerBlock);

    const HeaderParams ct = parseParams(p.headers.raw("Content-Type"));
    p.mimeType = ct.value.isEmpty() || !ct.value.contains('/') ? defaultType : ct.value;
    p.typeParams = ct.params;

    const HeaderParams cd = parseParams(p.headers.raw("Content-Disposition"));
    p.disposition = cd.value;
    p.filename = cd.params.value("filename");
    if (p.filename.isEmpty())
        p.filename = p.typeParams.value("name");
    const QStringList cid = parseMessageIds(p.headers.raw("Content-ID"));
    if (!cid.isEmpty())
        p.contentId = cid.first();

    const QByteArray boundary = p.typeParams.value("boundary").toUtf8();
    if (p.isMultipart() && !boundary.isEmpty() && depth < kMaxDepth) {
        const QByteArray childDefault = p.mimeType == "multipart/digest" ? "message/rfc822" : "text/plain";
        for (const QByteArray &chunk : splitMultipart(body, boundary))
            p.children.append(parseEntity(chunk, depth + 1, childDefault));
        return p;
    }
    if (p.isMultipart())
        p.mimeType = "text/plain"; // no usable boundary: show it as text
    p.body = decodeTransfer(body, p.headers.raw("Content-Transfer-Encoding"));
    return p;
}

} // namespace

void splitHeaderBody(const QByteArray &raw, QByteArray *headers, QByteArray *body)
{
    // An entity may begin with its body when it has no headers at all.
    if (raw.startsWith("\r\n") || raw.startsWith('\n')) {
        headers->clear();
        *body = raw.mid(raw.startsWith("\r\n") ? 2 : 1);
        return;
    }
    const qsizetype crlf = raw.indexOf("\r\n\r\n");
    const qsizetype lf = raw.indexOf("\n\n");
    if (crlf >= 0 && (lf < 0 || crlf < lf)) {
        *headers = raw.left(crlf);
        *body = raw.mid(crlf + 4);
    } else if (lf >= 0) {
        *headers = raw.left(lf);
        *body = raw.mid(lf + 2);
    } else {
        *headers = raw;
        body->clear();
    }
}

QString Part::text() const
{
    return decodeCharset(body, charset());
}

Part parse(const QByteArray &raw)
{
    return parseEntity(raw, 0, "text/plain");
}

} // namespace e3::mime
