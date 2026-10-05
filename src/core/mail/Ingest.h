// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Types.h"

#include "mime/Part.h"

namespace e3::mail {

// The receive path: one raw message in, one stored message out. Decrypts,
// verifies, learns keys, gates strangers, threads and indexes, all in one
// transaction, so a crash leaves either the whole message or none of it.
class Ingest
{
public:
    struct Result
    {
        qint64 msgId = 0;
        bool duplicate = false;
        bool held = false;
        bool trashed = false;
    };

    static Result process(MailContext &ctx, const QByteArray &raw);

    // What a parsed message body comes to, independent of storage.
    struct Content
    {
        QString text;
        QString html;
        struct Attachment
        {
            QString filename;
            QString mimeType;
            QString contentId;
            QByteArray data;
        };
        QList<Attachment> attachments;
    };
    static Content extract(const mime::Part &root);

    // Importance: Importance, then X-Priority, then Priority; an unknown
    // value says nothing and the next header is tried.
    static int importanceOf(const mime::HeaderList &h);

    static QString makePreview(const QString &text);
};

} // namespace e3::mail
