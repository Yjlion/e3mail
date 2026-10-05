// SPDX-License-Identifier: MPL-2.0
#include "Builder.h"

#include "Encoding.h"

#include <QMimeDatabase>
#include <QRandomGenerator>

namespace e3::mime {

namespace {

QByteArray textPart(const QString &text, const QByteArray &subtype)
{
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    const QByteArray utf8 = normalized.toUtf8();
    QByteArray out = "Content-Type: text/" + subtype + "; charset=utf-8\r\n";
    // 7bit when possible keeps mail readable in a raw view; QP otherwise.
    bool plain7bit = isAscii(utf8);
    for (const QByteArray &line : utf8.split('\n')) {
        if (line.size() > 76 || line.startsWith("From ") || line.startsWith('.') || line.endsWith(' '))
            plain7bit = false;
    }
    if (plain7bit) {
        out += "Content-Transfer-Encoding: 7bit\r\n\r\n";
        QByteArray body = utf8;
        body.replace("\n", "\r\n");
        out += body;
    } else {
        out += "Content-Transfer-Encoding: quoted-printable\r\n\r\n";
        out += encodeQuotedPrintable(utf8);
    }
    return out;
}

QByteArray attachmentPart(const OutgoingAttachment &a)
{
    QByteArray type = a.mimeType;
    if (type.isEmpty())
        type = QMimeDatabase().mimeTypeForFileNameAndData(a.filename, a.data).name().toLatin1();
    QByteArray out = "Content-Type: " + type + "; " + formatParam("name", a.filename) + "\r\n";
    out += "Content-Disposition: attachment; " + formatParam("filename", a.filename) + "\r\n";
    out += "Content-Transfer-Encoding: base64\r\n\r\n";
    out += encodeBase64Lines(a.data);
    return out;
}

QByteArray multipart(const QByteArray &subtype, const QList<QByteArray> &parts,
                     const QByteArray &extraTypeParams = {})
{
    const QByteArray boundary = Builder::newBoundary();
    QByteArray out = "Content-Type: multipart/" + subtype + "; boundary=\"" + boundary + '"'
        + extraTypeParams + "\r\n\r\n";
    for (const QByteArray &p : parts)
        out += "--" + boundary + "\r\n" + p + "\r\n";
    out += "--" + boundary + "--\r\n";
    return out;
}

void appendImportance(QByteArray &out, int importance)
{
    // Absent when normal; written both ways because clients read different ones.
    if (importance > 0)
        out += "Importance: high\r\nX-Priority: 1 (Highest)\r\n";
    else if (importance < 0)
        out += "Importance: low\r\nX-Priority: 5 (Lowest)\r\n";
}

} // namespace

QByteArray Builder::newBoundary()
{
    const quint64 a = QRandomGenerator::system()->generate64();
    const quint64 b = QRandomGenerator::system()->generate64();
    return "e3-" + QByteArray::number(a, 36) + QByteArray::number(b, 36);
}

QByteArray Builder::contentEntity(const OutgoingMail &mail, bool protectedHeaders)
{
    QByteArray body = textPart(mail.text, "plain");
    if (!mail.html.isEmpty())
        body = multipart("alternative", {body, textPart(mail.html, "html")});
    if (!mail.attachments.isEmpty()) {
        QList<QByteArray> parts{body};
        for (const OutgoingAttachment &a : mail.attachments)
            parts.append(attachmentPart(a));
        body = multipart("mixed", parts);
    }
    if (!protectedHeaders)
        return body;

    // RFC 9788: wrap in multipart/mixed carrying protected-headers="v1" and
    // copies of the real headers, so the outer ones can be minimised.
    QByteArray hdr;
    hdr += foldHeader("From", formatAddress(mail.from));
    if (!mail.to.isEmpty())
        hdr += foldHeader("To", formatAddressList(mail.to));
    if (!mail.cc.isEmpty())
        hdr += foldHeader("Cc", formatAddressList(mail.cc));
    hdr += foldHeader("Subject", encodeHeader(mail.subject));
    hdr += "Date: " + formatDate(mail.date) + "\r\n";
    hdr += "Message-ID: <" + mail.messageId.toUtf8() + ">\r\n";
    if (!mail.inReplyTo.isEmpty())
        hdr += "In-Reply-To: <" + mail.inReplyTo.toUtf8() + ">\r\n";
    if (!mail.references.isEmpty())
        hdr += foldHeader("References", "<" + mail.references.join(QStringLiteral("> <")).toUtf8() + ">");
    appendImportance(hdr, mail.importance);
    for (const auto &[name, value] : mail.extraHeaders) {
        if (name.startsWith("Autocrypt-Gossip"))
            hdr += foldHeader(name, value);
    }

    const QByteArray boundary = newBoundary();
    QByteArray out = "Content-Type: multipart/mixed; boundary=\"" + boundary
        + "\"; protected-headers=\"v1\"\r\n" + hdr + "\r\n";
    out += "--" + boundary + "\r\n" + body + "\r\n--" + boundary + "--\r\n";
    return out;
}

QByteArray Builder::headerBlock(const OutgoingMail &mail, const QString *subjectOverride)
{
    QByteArray out;
    out += foldHeader("From", formatAddress(mail.from));
    if (!mail.to.isEmpty())
        out += foldHeader("To", formatAddressList(mail.to));
    if (!mail.cc.isEmpty())
        out += foldHeader("Cc", formatAddressList(mail.cc));
    out += foldHeader("Subject", encodeHeader(subjectOverride ? *subjectOverride : mail.subject));
    out += "Date: " + formatDate(mail.date) + "\r\n";
    out += "Message-ID: <" + mail.messageId.toUtf8() + ">\r\n";
    if (!mail.inReplyTo.isEmpty())
        out += "In-Reply-To: <" + mail.inReplyTo.toUtf8() + ">\r\n";
    if (!mail.references.isEmpty())
        out += foldHeader("References", "<" + mail.references.join(QStringLiteral("> <")).toUtf8() + ">");
    if (!subjectOverride)
        appendImportance(out, mail.importance);
    for (const auto &[name, value] : mail.extraHeaders) {
        if (!name.startsWith("Autocrypt-Gossip"))
            out += foldHeader(name, value);
    }
    out += "MIME-Version: 1.0\r\n";
    return out;
}

QByteArray Builder::build(const OutgoingMail &mail)
{
    return headerBlock(mail) + contentEntity(mail);
}

} // namespace e3::mime
