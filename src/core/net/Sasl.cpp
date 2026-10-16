// SPDX-License-Identifier: MPL-2.0
#include "Sasl.h"

namespace e3::net::sasl {

QByteArray xoauth2(const QByteArray &user, const QByteArray &token)
{
    return ("user=" + user + "\x01" "auth=Bearer " + token + "\x01\x01").toBase64();
}

QByteArray oauthBearer(const QByteArray &user, const QByteArray &token, const QByteArray &host, quint16 port)
{
    return ("n,a=" + user + ",\x01" "host=" + host + "\x01" "port=" + QByteArray::number(port) + "\x01"
            "auth=Bearer " + token + "\x01\x01")
        .toBase64();
}

QByteArray abortResponse(bool bearer)
{
    // RFC 7628 3.2.3: a single ^A. XOAUTH2 takes an empty line.
    return bearer ? QByteArray("AQ==") : QByteArray();
}

QByteArray challengeText(const QByteArray &continuation)
{
    // "+ <base64>" (IMAP, POP3), or the base64 alone (an SMTP 334's text).
    QByteArray b64 = continuation.trimmed();
    if (b64.startsWith('+'))
        b64 = b64.mid(1).trimmed();
    return QByteArray::fromBase64(b64);
}

} // namespace e3::net::sasl
