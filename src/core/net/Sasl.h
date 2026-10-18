// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>

namespace e3::net::sasl {

// The initial responses for signing in with an OAuth 2 access token, already
// base64. XOAUTH2 is Google's and Microsoft's; OAUTHBEARER is RFC 7628's.
QByteArray xoauth2(const QByteArray &user, const QByteArray &token);
QByteArray oauthBearer(const QByteArray &user, const QByteArray &token, const QByteArray &host, quint16 port);
// What a client sends to a server's error challenge (a continuation holding
// JSON) so that the server ends the exchange with its failure.
QByteArray abortResponse(bool bearer);
// The JSON a server sends in that challenge, decoded, for the error message.
QByteArray challengeText(const QByteArray &continuation);

} // namespace e3::net::sasl
