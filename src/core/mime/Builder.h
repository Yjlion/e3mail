// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Headers.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <QString>

namespace e3::mime {

struct OutgoingAttachment
{
    QString filename;
    QByteArray mimeType;
    QByteArray data;
};

// Everything that goes into composing one message. Bcc is deliberately
// absent: it reaches the SMTP envelope and the key set, never a header.
struct OutgoingMail
{
    Address from;
    QList<Address> to;
    QList<Address> cc;
    QString subject;
    QDateTime date;
    QString messageId;
    QString inReplyTo;
    QStringList references;
    QString text;               // always sent
    QString html;               // optional alternative, already whitelisted
    QList<OutgoingAttachment> attachments;
    int importance = 0;         // -1 low, 0 normal (no header), 1 high
    QList<QPair<QByteArray, QByteArray>> extraHeaders; // e.g. Autocrypt
    // Only inside the encrypted part, with the protected headers; never on
    // the outside and never in cleartext mail (e.g. SecureJoin's tokens).
    QList<QPair<QByteArray, QByteArray>> innerHeaders;
};

class Builder
{
public:
    // The content entity: text, optional HTML alternative, attachments, with
    // its own Content-Type headers. `protectedHeaders` adds RFC 9788 header
    // copies inside the entity, for use as an encrypted payload.
    static QByteArray contentEntity(const OutgoingMail &mail, bool protectedHeaders = false);

    // A read receipt (RFC 8098) for `originalMessageId`, as the content
    // entity: `mail.text` is its human-readable part.
    static QByteArray receiptEntity(const OutgoingMail &mail, const QString &originalMessageId,
                                    const QString &finalRecipient, bool protectedHeaders);
    // `body` wrapped with RFC 9788 copies of the headers, for encryption.
    static QByteArray withProtectedHeaders(const OutgoingMail &mail, const QByteArray &body);

    // RFC 5322 header block, CRLF terminated, without the blank line.
    // `subjectOverride` replaces the subject on the outside of encrypted mail.
    static QByteArray headerBlock(const OutgoingMail &mail, const QString *subjectOverride = nullptr);

    // A complete cleartext message.
    static QByteArray build(const OutgoingMail &mail);

    static QByteArray newBoundary();
    // The Content-Type and body of a PGP/MIME encrypted message (RFC 3156)
    // around an armoured OpenPGP message; follows a header block.
    static QByteArray encryptedBody(const QByteArray &armored);
};

} // namespace e3::mime
