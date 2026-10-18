// SPDX-License-Identifier: MPL-2.0
#include "Http.h"
#include "net/SyncSocket.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSslCertificate>
#include <QSslError>

namespace e3::backup {

QByteArray HttpReply::header(const QByteArray &name) const
{
    for (const auto &[k, v] : headers) {
        if (k.compare(name, Qt::CaseInsensitive) == 0)
            return v;
    }
    return {};
}

HttpReply Http::request(const QByteArray &verb, const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers,
                        const QByteArray &body)
{
    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setTransferTimeout(timeoutMs);
    // Plain HTTP only where the person typed it (a server on their own
    // network); credentials never go to a redirect elsewhere.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    for (const auto &[k, v] : headers)
        req.setRawHeader(k, v);
    QBuffer data;
    data.setData(body);
    data.open(QIODevice::ReadOnly);
    QNetworkReply *reply = nam.sendCustomRequest(req, verb, &data);
    QSslCertificate refused;
    QStringList problems;
    QObject::connect(reply, &QNetworkReply::sslErrors, reply, [&, reply](const QList<QSslError> &errors) {
        const QSslCertificate cert = errors.isEmpty() ? QSslCertificate() : errors.first().certificate();
        if (!pinnedSha256.isEmpty() && !cert.isNull() && cert.digest(QCryptographicHash::Sha256).toHex() == pinnedSha256) {
            reply->ignoreSslErrors(errors);
            return;
        }
        refused = cert;
        for (const QSslError &e : errors)
            problems.append(e.errorString());
    });
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    HttpReply r;
    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    r.body = reply->readAll();
    for (const auto &h : reply->rawHeaderPairs())
        r.headers.append(h);
    const QString error = reply->errorString();
    delete reply;
    if (!refused.isNull()) {
        net::CertInfo info;
        info.host = url.host();
        info.port = quint16(url.port(url.scheme() == QLatin1String("https") ? 443 : 80));
        info.sha256 = refused.digest(QCryptographicHash::Sha256).toHex();
        info.subject = refused.subjectDisplayName();
        info.issuer = refused.issuerDisplayName();
        info.expires = refused.expiryDate();
        info.problems = problems;
        info.pinChanged = !pinnedSha256.isEmpty();
        throw net::CertError(info);
    }
    if (r.status == 0)
        throw net::NetError(QStringLiteral("%1: %2").arg(url.host(), error));
    return r;
}

} // namespace e3::backup
