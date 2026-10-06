// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>

namespace e3::mime {

QByteArray decodeQuotedPrintable(const QByteArray &in, bool headerMode = false);
// Encodes for a body part: CRLF line endings, soft breaks at 76 columns.
QByteArray encodeQuotedPrintable(const QByteArray &in);
// Base64 wrapped at 76 columns with CRLF.
QByteArray encodeBase64Lines(const QByteArray &in);
QByteArray decodeTransfer(const QByteArray &body, const QByteArray &cte);

// Decodes bytes in a named charset. Unknown or missing charsets fall back to
// UTF-8 if the bytes are valid UTF-8, and Windows-1252 otherwise, which is
// what mislabelled mail in the wild nearly always is.
QString decodeCharset(const QByteArray &bytes, const QByteArray &charset);

// RFC 2047: "=?utf-8?q?...?=" words, with whitespace between adjacent encoded
// words removed. Raw 8-bit header bytes are read as UTF-8.
QString decodeHeader(const QByteArray &raw);
// Returns the value unchanged when it is printable ASCII, otherwise UTF-8
// base64 encoded words of a length that fold cleanly.
QByteArray encodeHeader(const QString &value);
// Folds a full "Name: value" header to lines of at most 78 columns where
// whitespace allows. Returns the line with a trailing CRLF.
QByteArray foldHeader(const QByteArray &name, const QByteArray &value);

// Parses a structured header such as Content-Type into its leading token
// (lower-cased) and parameters (names lower-cased), decoding RFC 2231
// continuations and charset-tagged values.
struct HeaderParams
{
    QByteArray value;
    QMap<QByteArray, QString> params;
};
HeaderParams parseParams(const QByteArray &raw);
// Quotes a parameter value when needed; non-ASCII values use RFC 2231.
QByteArray formatParam(const QByteArray &name, const QString &value);

bool isAscii(const QByteArray &bytes);

} // namespace e3::mime
