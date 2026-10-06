// SPDX-License-Identifier: MPL-2.0
#include "Encoding.h"

#include <QStringDecoder>
#include <QUrl>

#include <unicode/ucnv.h>

#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <optional>

namespace e3::mime {

namespace {

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

// Windows-1252 0x80..0x9F; the rest of the range is ISO-8859-1.
constexpr std::array<char16_t, 32> kCp1252High = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
};

QString decodeCp1252(const QByteArray &bytes)
{
    QString out;
    out.reserve(bytes.size());
    for (char ch : bytes) {
        const auto b = static_cast<unsigned char>(ch);
        if (b >= 0x80 && b < 0xA0)
            out.append(QChar(kCp1252High[b - 0x80]));
        else
            out.append(QChar(b));
    }
    return out;
}

bool isValidUtf8(const QByteArray &bytes)
{
    QStringDecoder dec(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString s = dec(bytes);
    return !dec.hasError();
}

// Any charset ICU knows, by any of its aliases. With `strict`, invalid input
// is a failure rather than U+FFFD, so a mislabelled message can be retried.
std::optional<QString> icuDecode(const QByteArray &bytes, const char *charset, bool strict)
{
    UErrorCode err = U_ZERO_ERROR;
    std::unique_ptr<UConverter, decltype(&ucnv_close)> conv(ucnv_open(charset, &err), ucnv_close);
    if (U_FAILURE(err) || !conv)
        return std::nullopt;
    if (strict)
        ucnv_setToUCallBack(conv.get(), UCNV_TO_U_CALLBACK_STOP, nullptr, nullptr, nullptr, &err);
    // No charset yields more than two UTF-16 units per input byte.
    QString out(bytes.size() * 2 + 1, Qt::Uninitialized);
    const int32_t n = ucnv_toUChars(conv.get(), reinterpret_cast<UChar *>(out.data()), int32_t(out.size()),
                                    bytes.constData(), int32_t(bytes.size()), &err);
    if (U_FAILURE(err))
        return std::nullopt;
    out.truncate(n);
    return out;
}

bool isTSpecial(char c)
{
    return std::strchr("()<>@,;:\\\"/[]?= \t", c) != nullptr;
}

} // namespace

bool isAscii(const QByteArray &bytes)
{
    for (char c : bytes) {
        if (static_cast<unsigned char>(c) >= 0x80)
            return false;
    }
    return true;
}

QByteArray decodeQuotedPrintable(const QByteArray &in, bool headerMode)
{
    QByteArray out;
    out.reserve(in.size());
    for (qsizetype i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (headerMode && c == '_') {
            out.append(' ');
        } else if (c == '=') {
            // Soft line break: "=" then optional whitespace then a newline.
            qsizetype j = i + 1;
            while (j < in.size() && (in[j] == ' ' || in[j] == '\t'))
                ++j;
            if (j < in.size() && (in[j] == '\r' || in[j] == '\n')) {
                if (in[j] == '\r' && j + 1 < in.size() && in[j + 1] == '\n')
                    ++j;
                i = j;
                continue;
            }
            if (j >= in.size()) {
                i = j;
                continue;
            }
            if (i + 2 < in.size() && hexValue(in[i + 1]) >= 0 && hexValue(in[i + 2]) >= 0) {
                out.append(char(hexValue(in[i + 1]) * 16 + hexValue(in[i + 2])));
                i += 2;
            } else {
                out.append('='); // malformed: keep literally
            }
        } else {
            out.append(c);
        }
    }
    return out;
}

QByteArray encodeQuotedPrintable(const QByteArray &in)
{
    static const char hex[] = "0123456789ABCDEF";
    QByteArray out;
    int col = 0;
    auto emit = [&](const QByteArray &chunk) {
        if (col + chunk.size() > 75) {
            out.append("=\r\n");
            col = 0;
        }
        out.append(chunk);
        col += int(chunk.size());
    };
    for (qsizetype i = 0; i < in.size(); ++i) {
        const auto c = static_cast<unsigned char>(in[i]);
        if (c == '\r' && i + 1 < in.size() && in[i + 1] == '\n')
            continue; // handled with the '\n'
        if (c == '\n') {
            // Trailing whitespace before a hard break must be encoded.
            if (!out.isEmpty() && (out.endsWith(' ') || out.endsWith('\t'))) {
                const char ws = out.back();
                out.chop(1);
                --col;
                emit(QByteArray("=") + hex[ws >> 4] + hex[ws & 15]);
            }
            out.append("\r\n");
            col = 0;
            continue;
        }
        const bool literal = (c >= 33 && c <= 126 && c != '=') || c == ' ' || c == '\t';
        // A leading "." or "From " is encoded to survive naive transports.
        const bool lineStart = col == 0;
        if (literal && !(lineStart && (c == '.' || (c == 'F' && in.mid(i, 5) == "From "))))
            emit(QByteArray(1, char(c)));
        else
            emit(QByteArray("=") + hex[c >> 4] + hex[c & 15]);
    }
    if (out.endsWith(' ') || out.endsWith('\t')) {
        const char ws = out.back();
        out.chop(1);
        --col;
        emit(QByteArray("=") + hex[ws >> 4] + hex[ws & 15]);
    }
    return out;
}

QByteArray encodeBase64Lines(const QByteArray &in)
{
    const QByteArray b64 = in.toBase64();
    QByteArray out;
    out.reserve(b64.size() + b64.size() / 76 * 2 + 2);
    for (qsizetype i = 0; i < b64.size(); i += 76) {
        out.append(b64.mid(i, 76));
        out.append("\r\n");
    }
    return out;
}

QByteArray decodeTransfer(const QByteArray &body, const QByteArray &cte)
{
    const QByteArray enc = cte.trimmed().toLower();
    if (enc == "base64")
        return QByteArray::fromBase64(body);
    if (enc == "quoted-printable")
        return decodeQuotedPrintable(body);
    return body;
}

QString decodeCharset(const QByteArray &bytes, const QByteArray &charset)
{
    QByteArray cs = charset.trimmed().toLower();
    if (cs.startsWith('"') && cs.endsWith('"') && cs.size() >= 2)
        cs = cs.mid(1, cs.size() - 2);
    if (cs.isEmpty() || cs == "us-ascii" || cs == "ascii" || cs == "utf8" || cs == "utf-8"
        || cs == "unknown-8bit" || cs == "x-unknown") {
        return isValidUtf8(bytes) ? QString::fromUtf8(bytes) : decodeCp1252(bytes);
    }
    if (cs == "iso-8859-1" || cs == "latin1" || cs == "windows-1252" || cs == "cp1252")
        return decodeCp1252(bytes); // senders that say latin1 mean cp1252
    if (const auto s = icuDecode(bytes, cs.constData(), true))
        return *s;
    // Mislabelled: often UTF-8 in truth. Otherwise the declared charset with
    // replacement characters beats guessing a Western one.
    if (isValidUtf8(bytes))
        return QString::fromUtf8(bytes);
    if (const auto s = icuDecode(bytes, cs.constData(), false))
        return *s;
    return decodeCp1252(bytes);
}

QString decodeHeader(const QByteArray &raw)
{
    QString out;
    qsizetype i = 0;
    bool lastWasEncoded = false;
    QString pendingSpace;
    while (i < raw.size()) {
        const qsizetype start = raw.indexOf("=?", i);
        if (start < 0) {
            out += pendingSpace;
            out += decodeCharset(raw.mid(i), QByteArray());
            break;
        }
        // Parse =?charset?enc?text?=
        const qsizetype q1 = raw.indexOf('?', start + 2);
        const qsizetype q2 = q1 < 0 ? -1 : raw.indexOf('?', q1 + 1);
        const qsizetype end = q2 < 0 ? -1 : raw.indexOf("?=", q2 + 1);
        if (q1 < 0 || q2 != q1 + 2 || end < 0) {
            out += pendingSpace;
            pendingSpace.clear();
            out += decodeCharset(raw.mid(i, start + 2 - i), QByteArray());
            i = start + 2;
            lastWasEncoded = false;
            continue;
        }
        const QByteArray between = raw.mid(i, start - i);
        if (lastWasEncoded && between.trimmed().isEmpty()) {
            // whitespace between adjacent encoded words is dropped
        } else {
            out += pendingSpace;
            out += decodeCharset(between, QByteArray());
        }
        pendingSpace.clear();
        QByteArray charset = raw.mid(start + 2, q1 - start - 2);
        if (const qsizetype star = charset.indexOf('*'); star >= 0)
            charset.truncate(star); // RFC 2231 language suffix
        const char enc = char(std::tolower(raw[q1 + 1]));
        const QByteArray text = raw.mid(q2 + 1, end - q2 - 1);
        const QByteArray bytes = enc == 'b' ? QByteArray::fromBase64(text) : decodeQuotedPrintable(text, true);
        out += decodeCharset(bytes, charset);
        lastWasEncoded = true;
        i = end + 2;
    }
    return out;
}

QByteArray encodeHeader(const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    bool plain = true;
    for (char c : utf8) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x80 || (u < 0x20 && u != '\t')) {
            plain = false;
            break;
        }
    }
    if (plain && !utf8.contains("=?"))
        return utf8;
    // Encode in chunks of whole characters so no word splits a UTF-8 sequence.
    QByteArray out;
    QByteArray chunk;
    auto flush = [&] {
        if (chunk.isEmpty())
            return;
        if (!out.isEmpty())
            out.append(' ');
        out.append("=?UTF-8?B?" + chunk.toBase64() + "?=");
        chunk.clear();
    };
    for (qsizetype i = 0; i < value.size(); ++i) {
        QString ch(value[i]);
        if (value[i].isHighSurrogate() && i + 1 < value.size())
            ch += value[++i];
        const QByteArray b = ch.toUtf8();
        if (chunk.size() + b.size() > 45)
            flush();
        chunk.append(b);
    }
    flush();
    return out;
}

QByteArray foldHeader(const QByteArray &name, const QByteArray &value)
{
    QByteArray line = name + ": ";
    QByteArray out;
    int col = int(line.size());
    out = line;
    const QList<QByteArray> words = value.split(' ');
    bool first = true;
    for (const QByteArray &w : words) {
        if (!first) {
            if (col + 1 + w.size() > 78) {
                out.append("\r\n ");
                col = 1;
            } else {
                out.append(' ');
                ++col;
            }
        }
        out.append(w);
        col += int(w.size());
        first = false;
    }
    out.append("\r\n");
    return out;
}

HeaderParams parseParams(const QByteArray &raw)
{
    HeaderParams result;
    // Split on ';' outside quotes.
    QList<QByteArray> parts;
    QByteArray cur;
    bool inQuote = false;
    for (qsizetype i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '\\' && inQuote && i + 1 < raw.size()) {
            cur.append(c);
            cur.append(raw[++i]);
            continue;
        }
        if (c == '"')
            inQuote = !inQuote;
        if (c == ';' && !inQuote) {
            parts.append(cur);
            cur.clear();
        } else {
            cur.append(c);
        }
    }
    parts.append(cur);
    result.value = parts.takeFirst().trimmed().toLower();

    struct Piece
    {
        QByteArray value;
        bool extended;
    };
    QMap<QByteArray, QMap<int, Piece>> continued;
    for (const QByteArray &p : std::as_const(parts)) {
        const qsizetype eq = p.indexOf('=');
        if (eq < 0)
            continue;
        QByteArray name = p.left(eq).trimmed().toLower();
        QByteArray val = p.mid(eq + 1).trimmed();
        if (val.startsWith('"')) {
            QByteArray unq;
            for (qsizetype i = 1; i < val.size(); ++i) {
                if (val[i] == '\\' && i + 1 < val.size())
                    unq.append(val[++i]);
                else if (val[i] == '"')
                    break;
                else
                    unq.append(val[i]);
            }
            val = unq;
        }
        bool extended = false;
        if (name.endsWith('*')) {
            extended = true;
            name.chop(1);
        }
        int index = -1;
        if (const qsizetype star = name.indexOf('*'); star >= 0) {
            bool ok = false;
            index = name.mid(star + 1).toInt(&ok);
            if (!ok)
                continue;
            name.truncate(star);
        }
        if (index < 0 && !extended) {
            result.params.insert(name, decodeHeader(val)); // tolerate RFC 2047 in params
            continue;
        }
        continued[name].insert(index < 0 ? 0 : index, Piece{val, extended});
    }
    for (auto it = continued.cbegin(); it != continued.cend(); ++it) {
        QByteArray charset;
        QByteArray bytes;
        bool firstPiece = true;
        for (const Piece &piece : it.value()) {
            QByteArray v = piece.value;
            if (piece.extended) {
                if (firstPiece) {
                    // charset'language'percent-encoded
                    const qsizetype a = v.indexOf('\'');
                    const qsizetype b = a < 0 ? -1 : v.indexOf('\'', a + 1);
                    if (a >= 0 && b >= 0) {
                        charset = v.left(a);
                        v = v.mid(b + 1);
                    }
                }
                v = QByteArray::fromPercentEncoding(v);
            }
            bytes.append(v);
            firstPiece = false;
        }
        result.params.insert(it.key(), decodeCharset(bytes, charset));
    }
    return result;
}

QByteArray formatParam(const QByteArray &name, const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    if (!isAscii(utf8))
        return name + "*=UTF-8''" + utf8.toPercentEncoding("!#$&+-.^_`|~");
    bool needsQuote = utf8.isEmpty();
    for (char c : utf8) {
        if (isTSpecial(c) || static_cast<unsigned char>(c) < 0x20)
            needsQuote = true;
    }
    if (!needsQuote)
        return name + '=' + utf8;
    QByteArray q = utf8;
    q.replace('\\', "\\\\").replace('"', "\\\"");
    return name + "=\"" + q + '"';
}

} // namespace e3::mime
