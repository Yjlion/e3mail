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
};

class Builder
{
public:
    // The content entity: text, optional HTML alternative, attachments, with
    // its own Content-Type headers. `protectedHeaders` adds RFC 9788 header
    // copies inside the entity, for use as an encrypted payload.
    static QByteArray contentEntity(const OutgoingMail &mail, bool protectedHeaders = false);

    // RFC 5322 header block, CRLF terminated, without the blank line.
    // `subjectOverride` replaces the subject on the outside of encrypted mail.
    static QByteArray headerBlock(const OutgoingMail &mail, const QString *subjectOverride = nullptr);

    // A complete cleartext message.
    static QByteArray build(const OutgoingMail &mail);

    static QByteArray newBoundary();
};

} // namespace e3::mime
