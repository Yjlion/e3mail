// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Headers.h"

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>

namespace e3::mime {

// One MIME entity: headers plus either a decoded body or child parts.
struct Part
{
    HeaderList headers;
    QByteArray mimeType;                 // lower case, "text/plain" when absent
    QMap<QByteArray, QString> typeParams;
    QByteArray disposition;              // lower case: "inline", "attachment" or empty
    QString filename;
    QString contentId;                   // without <>
    QByteArray body;                     // transfer-decoded bytes, for leaves
    QByteArray raw;                      // the entity exactly as received, headers included
    QList<Part> children;

    bool isMultipart() const { return mimeType.startsWith("multipart/"); }
    QString text() const;                // body decoded in its charset
    QByteArray charset() const { return typeParams.value("charset").toLatin1(); }
};

// Parses a message or entity. Never throws: malformed input yields the best
// tree it can, because the alternative is losing someone's mail.
Part parse(const QByteArray &raw);

// Splits raw bytes into header block and body at the first empty line.
void splitHeaderBody(const QByteArray &raw, QByteArray *headers, QByteArray *body);

} // namespace e3::mime
