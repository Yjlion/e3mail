// SPDX-License-Identifier: MPL-2.0
#include "WebDav.h"
#include "net/SyncSocket.h"

#include <QXmlStreamReader>

namespace e3::backup {

WebDavDestination::WebDavDestination(const QUrl &base, const QString &user, const QString &password,
                                     const QByteArray &pin)
    : m_base(base)
{
    if (!m_base.path().endsWith(u'/'))
        m_base.setPath(m_base.path() + u'/');
    if (!user.isEmpty())
        m_auth = "Basic " + (user + u':' + password).toUtf8().toBase64();
    m_http.pinnedSha256 = pin;
}

QUrl WebDavDestination::at(const QString &name) const
{
    return m_base.resolved(QUrl(name));
}

HttpReply WebDavDestination::call(const QByteArray &verb, const QString &name, const QByteArray &body,
                                  const QList<QPair<QByteArray, QByteArray>> &extra)
{
    QList<QPair<QByteArray, QByteArray>> h = extra;
    if (!m_auth.isEmpty())
        h.append({"Authorization", m_auth});
    HttpReply r = m_http.request(verb, at(name), h, body);
    if (r.status == 401 || r.status == 403)
        throw net::AuthError(QStringLiteral("WebDAV: %1 refused the user name or password").arg(m_base.host()));
    return r;
}

void WebDavDestination::prepare()
{
    for (const char *dir : {"", "objects/", "snapshots/"}) {
        const HttpReply r = call("MKCOL", QString::fromLatin1(dir));
        // 405: it is there already.
        if (r.status != 201 && r.status != 405 && r.status != 200)
            throw net::NetError(QStringLiteral("WebDAV: cannot create the folder (%1)").arg(r.status));
    }
}

void WebDavDestination::put(const QString &name, const QByteArray &data)
{
    const HttpReply r = call("PUT", name, data, {{"Content-Type", "application/octet-stream"}});
    if (r.status < 200 || r.status > 299)
        throw net::NetError(QStringLiteral("WebDAV: cannot store %1 (%2)").arg(name).arg(r.status));
}

std::optional<QByteArray> WebDavDestination::get(const QString &name)
{
    const HttpReply r = call("GET", name);
    if (r.status == 404)
        return std::nullopt;
    if (r.status != 200)
        throw net::NetError(QStringLiteral("WebDAV: cannot read %1 (%2)").arg(name).arg(r.status));
    return r.body;
}

QList<Entry> WebDavDestination::list(const QString &dir)
{
    const QByteArray body = R"(<?xml version="1.0"?><d:propfind xmlns:d="DAV:"><d:prop>)"
                            R"(<d:getcontentlength/><d:getlastmodified/><d:resourcetype/></d:prop></d:propfind>)";
    const HttpReply r = call("PROPFIND", dir + u'/', body,
                             {{"Depth", "1"}, {"Content-Type", "application/xml; charset=utf-8"}});
    if (r.status == 404)
        return {};
    if (r.status != 207)
        throw net::NetError(QStringLiteral("WebDAV: cannot list %1 (%2)").arg(dir).arg(r.status));
    return parseMultistatus(r.body);
}

QList<Entry> WebDavDestination::parseMultistatus(const QByteArray &xml)
{
    QList<Entry> out;
    QXmlStreamReader x(xml);
    Entry cur;
    bool collection = false;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            const auto n = x.name();
            if (n == QLatin1String("response")) {
                cur = Entry();
                collection = false;
            } else if (n == QLatin1String("href")) {
                QString href = QUrl::fromPercentEncoding(x.readElementText().toUtf8());
                if (href.endsWith(u'/'))
                    collection = true;
                cur.name = href.section(u'/', -1);
            } else if (n == QLatin1String("getcontentlength")) {
                cur.size = x.readElementText().toLongLong();
            } else if (n == QLatin1String("getlastmodified")) {
                // HTTP dates say "GMT", which Qt's RFC 2822 parser does not take.
                QString date = x.readElementText().trimmed();
                if (date.endsWith(QLatin1String(" GMT")))
                    date = date.chopped(4) + QStringLiteral(" +0000");
                cur.modified = QDateTime::fromString(date, Qt::RFC2822Date).toUTC();
            } else if (n == QLatin1String("collection")) {
                collection = true;
            }
        } else if (x.isEndElement() && x.name() == QLatin1String("response")) {
            if (!collection && !cur.name.isEmpty())
                out.append(cur);
        }
    }
    return out;
}

void WebDavDestination::remove(const QString &name)
{
    const HttpReply r = call("DELETE", name);
    if (r.status != 404 && (r.status < 200 || r.status > 299))
        throw net::NetError(QStringLiteral("WebDAV: cannot remove %1 (%2)").arg(name).arg(r.status));
}

} // namespace e3::backup
