// SPDX-License-Identifier: MPL-2.0
#include "S3.h"
#include "net/SyncSocket.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <algorithm>

namespace e3::backup {

namespace {

QByteArray sha256Hex(const QByteArray &d)
{
    return QCryptographicHash::hash(d, QCryptographicHash::Sha256).toHex();
}

QByteArray hmac(const QByteArray &key, const QByteArray &data)
{
    return QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256);
}

// RFC 3986 unreserved characters stay; everything else is %XX, as SigV4 asks.
QByteArray awsEncode(const QByteArray &s, bool slash)
{
    return QUrl::toPercentEncoding(QString::fromUtf8(s), slash ? QByteArray("/~") : QByteArray("~"));
}

} // namespace

QByteArray S3Destination::authorization(const QByteArray &verb, const QUrl &url,
                                        QList<QPair<QByteArray, QByteArray>> headers, const QByteArray &payloadSha256,
                                        const QDateTime &when, const QString &region, const QString &service,
                                        const QString &accessKey, const QString &secretKey)
{
    const QByteArray amzDate = when.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")).toLatin1();
    const QByteArray day = amzDate.left(8);
    // Canonical query: each name and value encoded, sorted by name then value.
    QList<QPair<QByteArray, QByteArray>> q;
    for (const auto &[k, v] : QUrlQuery(url).queryItems(QUrl::FullyDecoded))
        q.append({awsEncode(k.toUtf8(), false), awsEncode(v.toUtf8(), false)});
    std::sort(q.begin(), q.end());
    QByteArrayList qs;
    for (const auto &[k, v] : q)
        qs.append(k + '=' + v);
    // Canonical headers: lower-case names, trimmed values, sorted.
    for (auto &[k, v] : headers) {
        k = k.toLower();
        v = v.simplified();
    }
    std::sort(headers.begin(), headers.end());
    QByteArray canonicalHeaders;
    QByteArrayList names;
    for (const auto &[k, v] : headers) {
        canonicalHeaders += k + ':' + v + '\n';
        names.append(k);
    }
    const QByteArray signedHeaders = names.join(';');
    QByteArray path = awsEncode(url.path(QUrl::FullyDecoded).toUtf8(), true);
    if (path.isEmpty())
        path = "/";
    const QByteArray canonical = verb + '\n' + path + '\n' + qs.join('&') + '\n' + canonicalHeaders + '\n'
                                 + signedHeaders + '\n' + payloadSha256;
    const QByteArray scope = day + '/' + region.toUtf8() + '/' + service.toUtf8() + "/aws4_request";
    const QByteArray toSign = "AWS4-HMAC-SHA256\n" + amzDate + '\n' + scope + '\n' + sha256Hex(canonical);
    QByteArray key = hmac("AWS4" + secretKey.toUtf8(), day);
    key = hmac(key, region.toUtf8());
    key = hmac(key, service.toUtf8());
    key = hmac(key, "aws4_request");
    const QByteArray signature = hmac(key, toSign).toHex();
    return "AWS4-HMAC-SHA256 Credential=" + accessKey.toUtf8() + '/' + scope + ", SignedHeaders=" + signedHeaders
           + ", Signature=" + signature;
}

S3Destination::S3Destination(const Options &o) : m_o(o)
{
    m_http.pinnedSha256 = o.pin;
}

QUrl S3Destination::urlFor(const QString &key) const
{
    QUrl u = m_o.endpoint;
    // An empty key: the bucket itself, for listing and making it.
    QString path = key.isEmpty() || m_o.prefix.isEmpty() ? key : m_o.prefix + u'/' + key;
    if (m_o.pathStyle)
        u.setPath(u'/' + m_o.bucket + (path.isEmpty() ? QString() : u'/' + path));
    else {
        u.setHost(m_o.bucket + u'.' + u.host());
        u.setPath(u'/' + path);
    }
    return u;
}

HttpReply S3Destination::call(const QByteArray &verb, const QUrl &url, const QByteArray &body)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QByteArray payload = sha256Hex(body);
    QByteArray host = url.host().toUtf8();
    if (url.port() > 0 && url.port() != (url.scheme() == QLatin1String("https") ? 443 : 80))
        host += ':' + QByteArray::number(url.port());
    QList<QPair<QByteArray, QByteArray>> h = {
        {"host", host},
        {"x-amz-content-sha256", payload},
        {"x-amz-date", now.toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")).toLatin1()}};
    const QByteArray auth = authorization(verb, url, h, payload, now, m_o.region, QStringLiteral("s3"), m_o.accessKey,
                                          m_o.secretKey);
    h.removeFirst(); // Qt sets Host itself, to the same value
    h.append({"Authorization", auth});
    HttpReply r = m_http.request(verb, url, h, body);
    if (r.status == 403 && r.body.contains("SignatureDoesNotMatch"))
        throw net::AuthError(QStringLiteral("S3: the secret key does not match the access key"));
    if (r.status == 403 && r.body.contains("InvalidAccessKeyId"))
        throw net::AuthError(QStringLiteral("S3: unknown access key"));
    return r;
}

void S3Destination::prepare()
{
    // The bucket is the person's to create; say so if it is not there.
    QUrl u = urlFor(QString());
    u.setQuery(QStringLiteral("list-type=2&max-keys=1"));
    const HttpReply r = call("GET", u);
    if (r.status == 404) {
        // Made here when the keys may: some services make buckets only so.
        QByteArray body;
        if (m_o.region != QLatin1String("us-east-1"))
            body = "<CreateBucketConfiguration xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\"><LocationConstraint>"
                   + m_o.region.toUtf8() + "</LocationConstraint></CreateBucketConfiguration>";
        const HttpReply made = call("PUT", urlFor(QString()), body);
        if (made.status != 200)
            throw net::NetError(QStringLiteral("S3: there is no bucket %1, and it cannot be made (%2)").arg(m_o.bucket).arg(made.status));
        return;
    }
    if (r.status != 200)
        throw net::NetError(QStringLiteral("S3: cannot use the bucket (%1)").arg(r.status));
}

void S3Destination::put(const QString &name, const QByteArray &data)
{
    const HttpReply r = call("PUT", urlFor(name), data);
    if (r.status != 200)
        throw net::NetError(QStringLiteral("S3: cannot store %1 (%2)").arg(name).arg(r.status));
}

std::optional<QByteArray> S3Destination::get(const QString &name)
{
    const HttpReply r = call("GET", urlFor(name));
    if (r.status == 404)
        return std::nullopt;
    if (r.status != 200)
        throw net::NetError(QStringLiteral("S3: cannot read %1 (%2)").arg(name).arg(r.status));
    return r.body;
}

QList<Entry> S3Destination::list(const QString &dir)
{
    QList<Entry> out;
    const QString prefix = (m_o.prefix.isEmpty() ? QString() : m_o.prefix + u'/') + dir + u'/';
    QString token;
    do {
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("list-type"), QStringLiteral("2"));
        q.addQueryItem(QStringLiteral("prefix"), prefix);
        if (!token.isEmpty())
            q.addQueryItem(QStringLiteral("continuation-token"), token);
        QUrl u = urlFor(QString());
        u.setQuery(q);
        const HttpReply r = call("GET", u);
        if (r.status != 200)
            throw net::NetError(QStringLiteral("S3: cannot list %1 (%2)").arg(dir).arg(r.status));
        token.clear();
        QXmlStreamReader x(r.body);
        Entry cur;
        while (!x.atEnd()) {
            x.readNext();
            if (x.isEndElement() && x.name() == QLatin1String("Contents")) {
                if (!cur.name.isEmpty() && !cur.name.contains(u'/'))
                    out.append(cur);
                continue;
            }
            if (!x.isStartElement())
                continue;
            const auto n = x.name();
            if (n == QLatin1String("Contents"))
                cur = Entry();
            else if (n == QLatin1String("Key"))
                cur.name = x.readElementText().mid(prefix.size());
            else if (n == QLatin1String("Size"))
                cur.size = x.readElementText().toLongLong();
            else if (n == QLatin1String("LastModified"))
                cur.modified = QDateTime::fromString(x.readElementText(), Qt::ISODateWithMs).toUTC();
            else if (n == QLatin1String("NextContinuationToken"))
                token = x.readElementText();
        }
    } while (!token.isEmpty());
    return out;
}

void S3Destination::remove(const QString &name)
{
    const HttpReply r = call("DELETE", urlFor(name));
    if (r.status != 204 && r.status != 200 && r.status != 404)
        throw net::NetError(QStringLiteral("S3: cannot remove %1 (%2)").arg(name).arg(r.status));
}

} // namespace e3::backup
