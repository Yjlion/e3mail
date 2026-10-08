// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Types.h"

#include <QByteArray>
#include <QString>

namespace e3::mail {

// What leaves e3mail as a file at the person's request: a message's
// original, its decrypted form, and the address book. Writing a file is not
// network traffic, so ADR 0009's barriers are not involved.
class Export
{
public:
    struct Source
    {
        bool retained = false;   // false once raw-message retention dropped the original
        QByteArray raw;          // the message exactly as received or sent
        bool encrypted = false;  // PGP/MIME
        QByteArray inner;        // the decrypted entity, when encrypted and decryptable
        QString decryptError;
    };
    static Source source(MailContext &ctx, qint64 msgId);

    // The original as a .eml; empty when it is not retained.
    static QByteArray eml(MailContext &ctx, qint64 msgId);
    // A cleartext .eml of an encrypted message (mime::Source::decryptedEml);
    // empty when the message is not encrypted, not retained, or cannot be
    // decrypted.
    static QByteArray decryptedEml(MailContext &ctx, qint64 msgId);

    // A file name for the message, from its subject; never a path.
    static QString fileName(MailContext &ctx, qint64 msgId, const QString &suffix);
    // `base` made safe as a file name on every system, or `fallback`.
    static QString safeFileName(QString base, const QString &fallback, const QString &suffix);

    // The address book: people added, accepted or written to, not everyone
    // ever seen on a message.
    static QList<ContactInfo> addressBook(MailContext &ctx);
};

} // namespace e3::mail
