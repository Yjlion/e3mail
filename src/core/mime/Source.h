// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace e3::mime {

// Reading and exporting a message's source: its structure, a shortened view
// for people, and a decrypted copy other mail programs can open.
class Source
{
public:
    struct Node
    {
        int depth = 0;
        QByteArray mimeType;
        QByteArray encoding;   // Content-Transfer-Encoding, lower case; empty when absent
        QString filename;
        qint64 size = 0;       // decoded bytes, for leaves; the entity's bytes for multiparts
    };

    // The MIME tree, depth first.
    static QList<Node> outline(const QByteArray &raw);

    // Collapses each run of more than four lines of base64 (attachments,
    // armour, encrypted data) into one marker line naming its size, so the
    // headers and text around it can be read. Everything else is unchanged.
    // The marker is wrapped in U+2068…U+2069 (a bidi isolate).
    static QString shorten(const QString &source);

    // A cleartext message from an encrypted one: the outer headers, minus the
    // MIME ones and those the inner part protects (RFC 9788: inner copies
    // win, as when receiving), followed by the decrypted entity whole.
    static QByteArray decryptedEml(const QByteArray &outer, const QByteArray &inner);
};

} // namespace e3::mime
