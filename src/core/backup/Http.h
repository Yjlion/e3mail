// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QUrl>

namespace e3::backup {

// One HTTP request, blocking the calling thread on a local event loop (ADR
// 0011), for the backup destinations. TLS as everywhere else in e3mail: the
// system's judgement, or exactly the pinned certificate (ADR 0017), whose
// refusal is a net::CertError carrying the certificate.
struct HttpReply
{
    int status = 0; // 0: no answer
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray header(const QByteArray &name) const;
};

class Http
{
public:
    QByteArray pinnedSha256;
    int timeoutMs = 120000;

    // Throws net::NetError when there is no answer at all.
    HttpReply request(const QByteArray &verb, const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers = {},
                      const QByteArray &body = {});
};

} // namespace e3::backup
