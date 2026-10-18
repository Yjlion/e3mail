// SPDX-License-Identifier: MPL-2.0
#include "Cloud.h"
#include "net/SyncSocket.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>

namespace e3::backup {

namespace {

QJsonObject json(const HttpReply &r)
{
    return QJsonDocument::fromJson(r.body).object();
}

QByteArray toJson(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// Drive's query language quotes with ' and escapes it with a backslash.
QString quoted(QString s)
{
    s.replace(u'\\', QStringLiteral("\\\\")).replace(u'\'', QStringLiteral("\\'"));
    return u'\'' + s + u'\'';
}

const QString kFolderType = QStringLiteral("application/vnd.google-apps.folder");

} // namespace

// --- Google Drive ---

DriveDestination::DriveDestination(std::shared_ptr<net::oauth::TokenSource> tokens, const QString &folder, const QUrl &api)
    : m_tokens(std::move(tokens)), m_folder(folder), m_api(api)
{
}

QUrl DriveDestination::url(const QString &path, const QList<QPair<QString, QString>> &query) const
{
    QUrl u = m_api;
    u.setPath(m_api.path() + path);
    QUrlQuery q;
    for (const auto &[k, v] : query)
        q.addQueryItem(k, QString::fromLatin1(QUrl::toPercentEncoding(v)));
    u.setQuery(q);
    return u;
}

HttpReply DriveDestination::call(const QByteArray &verb, const QUrl &u, const QByteArray &body,
                                 const QList<QPair<QByteArray, QByteArray>> &extra)
{
    for (bool fresh : {false, true}) {
        QList<QPair<QByteArray, QByteArray>> h = extra;
        h.append({"Authorization", "Bearer " + m_tokens->accessToken(fresh).toUtf8()});
        HttpReply r = m_http.request(verb, u, h, body);
        if (r.status != 401)
            return r;
    }
    throw net::AuthError(QStringLiteral("Google Drive refused the sign-in. Sign in again."));
}

QString DriveDestination::folderId(const QString &name, const QString &parent, bool create)
{
    const QString q = QStringLiteral("name = %1 and mimeType = %2 and %3 in parents and trashed = false")
                          .arg(quoted(name), quoted(kFolderType), quoted(parent));
    const HttpReply r = call("GET", url(QStringLiteral("/drive/v3/files"), {{QStringLiteral("q"), q},
                                                                         {QStringLiteral("fields"), QStringLiteral("files(id)")}}));
    if (r.status != 200)
        throw net::NetError(QStringLiteral("Google Drive: cannot look for %1 (%2)").arg(name).arg(r.status));
    const QJsonArray files = json(r).value(QLatin1String("files")).toArray();
    if (!files.isEmpty())
        return files.first().toObject().value(QLatin1String("id")).toString();
    if (!create)
        return {};
    const HttpReply c = call("POST", url(QStringLiteral("/drive/v3/files"), {{QStringLiteral("fields"), QStringLiteral("id")}}),
                             toJson({{QStringLiteral("name"), name},
                                     {QStringLiteral("mimeType"), kFolderType},
                                     {QStringLiteral("parents"), QJsonArray{parent}}}),
                             {{"Content-Type", "application/json"}});
    if (c.status != 200)
        throw net::NetError(QStringLiteral("Google Drive: cannot create %1 (%2)").arg(name).arg(c.status));
    return json(c).value(QLatin1String("id")).toString();
}

void DriveDestination::prepare()
{
    m_dirs.insert(QString(), folderId(m_folder, QStringLiteral("root"), true));
    for (const QString &d : {QStringLiteral("objects"), QStringLiteral("snapshots")})
        m_dirs.insert(d, folderId(d, m_dirs.value(QString()), true));
}

QString DriveDestination::parentOf(const QString &name)
{
    const QString dir = name.contains(u'/') ? name.section(u'/', 0, 0) : QString();
    if (!m_dirs.contains(dir)) {
        // Found, not made: a repository that is not there is not there.
        const QString top = m_dirs.contains(QString()) ? m_dirs.value(QString())
                                                        : folderId(m_folder, QStringLiteral("root"), false);
        if (top.isEmpty())
            return {};
        m_dirs.insert(QString(), top);
        if (!dir.isEmpty()) {
            const QString id = folderId(dir, top, false);
            if (id.isEmpty())
                return {};
            m_dirs.insert(dir, id);
        }
    }
    return m_dirs.value(dir);
}

QString DriveDestination::idOf(const QString &name)
{
    if (m_ids.contains(name))
        return m_ids.value(name);
    const QString parent = parentOf(name);
    if (parent.isEmpty())
        return {};
    const QString q = QStringLiteral("name = %1 and %2 in parents and trashed = false")
                          .arg(quoted(name.section(u'/', -1)), quoted(parent));
    const HttpReply r = call("GET", url(QStringLiteral("/drive/v3/files"), {{QStringLiteral("q"), q},
                                                                         {QStringLiteral("fields"), QStringLiteral("files(id)")}}));
    if (r.status != 200)
        throw net::NetError(QStringLiteral("Google Drive: cannot look for %1 (%2)").arg(name).arg(r.status));
    const QJsonArray files = json(r).value(QLatin1String("files")).toArray();
    const QString id = files.isEmpty() ? QString() : files.first().toObject().value(QLatin1String("id")).toString();
    if (!id.isEmpty())
        m_ids.insert(name, id);
    return id;
}

void DriveDestination::put(const QString &name, const QByteArray &data)
{
    const QString existing = idOf(name);
    // A resumable upload: one request for where, one for the bytes.
    HttpReply start;
    if (existing.isEmpty()) {
        const QString parent = parentOf(name);
        if (parent.isEmpty())
            throw net::NetError(QStringLiteral("Google Drive: the backup folder is gone"));
        start = call("POST", url(QStringLiteral("/upload/drive/v3/files"), {{QStringLiteral("uploadType"), QStringLiteral("resumable")}}),
                     toJson({{QStringLiteral("name"), name.section(u'/', -1)}, {QStringLiteral("parents"), QJsonArray{parent}}}),
                     {{"Content-Type", "application/json"}, {"X-Upload-Content-Length", QByteArray::number(data.size())}});
    } else {
        start = call("PATCH", url(QStringLiteral("/upload/drive/v3/files/") + existing,
                                  {{QStringLiteral("uploadType"), QStringLiteral("resumable")}}),
                     "{}", {{"Content-Type", "application/json"}, {"X-Upload-Content-Length", QByteArray::number(data.size())}});
    }
    const QByteArray location = start.header("Location");
    if (start.status != 200 || location.isEmpty())
        throw net::NetError(QStringLiteral("Google Drive: cannot store %1 (%2)").arg(name).arg(start.status));
    const HttpReply r = call("PUT", QUrl(QString::fromUtf8(location)), data, {{"Content-Type", "application/octet-stream"}});
    if (r.status != 200 && r.status != 201)
        throw net::NetError(QStringLiteral("Google Drive: cannot store %1 (%2)").arg(name).arg(r.status));
    const QString id = json(r).value(QLatin1String("id")).toString();
    if (!id.isEmpty())
        m_ids.insert(name, id);
}

std::optional<QByteArray> DriveDestination::get(const QString &name)
{
    const QString id = idOf(name);
    if (id.isEmpty())
        return std::nullopt;
    const HttpReply r = call("GET", url(QStringLiteral("/drive/v3/files/") + id, {{QStringLiteral("alt"), QStringLiteral("media")}}));
    if (r.status == 404) {
        m_ids.remove(name);
        return std::nullopt;
    }
    if (r.status != 200)
        throw net::NetError(QStringLiteral("Google Drive: cannot read %1 (%2)").arg(name).arg(r.status));
    return r.body;
}

QList<Entry> DriveDestination::list(const QString &dir)
{
    QList<Entry> out;
    const QString parent = parentOf(dir + QStringLiteral("/x"));
    if (parent.isEmpty())
        return out;
    QString page;
    do {
        QList<QPair<QString, QString>> q = {
            {QStringLiteral("q"), QStringLiteral("%1 in parents and trashed = false").arg(quoted(parent))},
            {QStringLiteral("fields"), QStringLiteral("nextPageToken,files(id,name,size,modifiedTime)")},
            {QStringLiteral("pageSize"), QStringLiteral("1000")}};
        if (!page.isEmpty())
            q.append({QStringLiteral("pageToken"), page});
        const HttpReply r = call("GET", url(QStringLiteral("/drive/v3/files"), q));
        if (r.status != 200)
            throw net::NetError(QStringLiteral("Google Drive: cannot list %1 (%2)").arg(dir).arg(r.status));
        const QJsonObject o = json(r);
        for (const QJsonValue &v : o.value(QLatin1String("files")).toArray()) {
            const QJsonObject f = v.toObject();
            const QString name = f.value(QLatin1String("name")).toString();
            m_ids.insert(dir + u'/' + name, f.value(QLatin1String("id")).toString());
            out.append({name, f.value(QLatin1String("size")).toString().toLongLong(),
                        QDateTime::fromString(f.value(QLatin1String("modifiedTime")).toString(), Qt::ISODateWithMs).toUTC()});
        }
        page = o.value(QLatin1String("nextPageToken")).toString();
    } while (!page.isEmpty());
    return out;
}

void DriveDestination::remove(const QString &name)
{
    const QString id = idOf(name);
    if (id.isEmpty())
        return;
    const HttpReply r = call("DELETE", url(QStringLiteral("/drive/v3/files/") + id));
    m_ids.remove(name);
    if (r.status != 204 && r.status != 200 && r.status != 404)
        throw net::NetError(QStringLiteral("Google Drive: cannot remove %1 (%2)").arg(name).arg(r.status));
}

// --- OneDrive ---

OneDriveDestination::OneDriveDestination(std::shared_ptr<net::oauth::TokenSource> tokens, const QUrl &api)
    : m_tokens(std::move(tokens)), m_api(api)
{
}

QUrl OneDriveDestination::item(const QString &path, const QString &suffix) const
{
    // The app's own folder: /me/drive/special/approot:/<path>:<suffix>
    QUrl u = m_api;
    QString p = m_api.path() + QStringLiteral("/me/drive/special/approot");
    if (!path.isEmpty())
        p += QStringLiteral(":/") + path + u':';
    u.setPath(p + suffix);
    return u;
}

HttpReply OneDriveDestination::call(const QByteArray &verb, const QUrl &u, const QByteArray &body,
                                    const QList<QPair<QByteArray, QByteArray>> &extra, bool auth)
{
    for (bool fresh : {false, true}) {
        QList<QPair<QByteArray, QByteArray>> h = extra;
        if (auth)
            h.append({"Authorization", "Bearer " + m_tokens->accessToken(fresh).toUtf8()});
        HttpReply r = m_http.request(verb, u, h, body);
        if (r.status != 401 || !auth)
            return r;
    }
    throw net::AuthError(QStringLiteral("OneDrive refused the sign-in. Sign in again."));
}

void OneDriveDestination::prepare()
{
    for (const QString &d : {QStringLiteral("objects"), QStringLiteral("snapshots")}) {
        const HttpReply r = call("POST", item(QString(), QStringLiteral("/children")),
                                 toJson({{QStringLiteral("name"), d},
                                         {QStringLiteral("folder"), QJsonObject()},
                                         {QStringLiteral("@microsoft.graph.conflictBehavior"), QStringLiteral("fail")}}),
                                 {{"Content-Type", "application/json"}});
        if (r.status != 201 && r.status != 200 && r.status != 409)
            throw net::NetError(QStringLiteral("OneDrive: cannot create %1 (%2)").arg(d).arg(r.status));
    }
}

void OneDriveDestination::put(const QString &name, const QByteArray &data)
{
    constexpr qsizetype kSimple = 4 << 20;
    if (data.size() <= kSimple) {
        const HttpReply r = call("PUT", item(name, QStringLiteral("/content")), data, {{"Content-Type", "application/octet-stream"}});
        if (r.status != 200 && r.status != 201)
            throw net::NetError(QStringLiteral("OneDrive: cannot store %1 (%2)").arg(name).arg(r.status));
        return;
    }
    // Larger: an upload session, in pieces of 10 MiB (a multiple of 320 KiB,
    // as Graph asks). The session's URL carries its own authorization.
    const HttpReply s = call("POST", item(name, QStringLiteral("/createUploadSession")),
                             toJson({{QStringLiteral("item"), QJsonObject{{QStringLiteral("@microsoft.graph.conflictBehavior"),
                                                                           QStringLiteral("replace")}}}}),
                             {{"Content-Type", "application/json"}});
    const QString upload = json(s).value(QLatin1String("uploadUrl")).toString();
    if (s.status != 200 || upload.isEmpty())
        throw net::NetError(QStringLiteral("OneDrive: cannot store %1 (%2)").arg(name).arg(s.status));
    constexpr qsizetype kPiece = 10 << 20;
    for (qsizetype at = 0; at < data.size(); at += kPiece) {
        const QByteArray piece = data.mid(at, kPiece);
        const QByteArray range = "bytes " + QByteArray::number(at) + '-' + QByteArray::number(at + piece.size() - 1) + '/'
                                 + QByteArray::number(data.size());
        const HttpReply r = call("PUT", QUrl(upload), piece, {{"Content-Range", range}}, false);
        if (r.status != 202 && r.status != 200 && r.status != 201)
            throw net::NetError(QStringLiteral("OneDrive: storing %1 failed (%2)").arg(name).arg(r.status));
    }
}

std::optional<QByteArray> OneDriveDestination::get(const QString &name)
{
    HttpReply r = call("GET", item(name, QStringLiteral("/content")));
    // The content is served from elsewhere, without the token.
    if (r.status == 302 || r.status == 301)
        r = call("GET", QUrl(QString::fromUtf8(r.header("Location"))), {}, {}, false);
    if (r.status == 404)
        return std::nullopt;
    if (r.status != 200)
        throw net::NetError(QStringLiteral("OneDrive: cannot read %1 (%2)").arg(name).arg(r.status));
    return r.body;
}

QList<Entry> OneDriveDestination::list(const QString &dir)
{
    QList<Entry> out;
    QUrl next = item(dir, QStringLiteral("/children"));
    next.setQuery(QStringLiteral("$select=name,size,lastModifiedDateTime,file&$top=1000"));
    while (next.isValid() && !next.isEmpty()) {
        const HttpReply r = call("GET", next);
        if (r.status == 404)
            return out;
        if (r.status != 200)
            throw net::NetError(QStringLiteral("OneDrive: cannot list %1 (%2)").arg(dir).arg(r.status));
        const QJsonObject o = json(r);
        for (const QJsonValue &v : o.value(QLatin1String("value")).toArray()) {
            const QJsonObject f = v.toObject();
            if (!f.contains(QLatin1String("file")))
                continue;
            out.append({f.value(QLatin1String("name")).toString(), qint64(f.value(QLatin1String("size")).toDouble()),
                        QDateTime::fromString(f.value(QLatin1String("lastModifiedDateTime")).toString(), Qt::ISODateWithMs).toUTC()});
        }
        next = QUrl(o.value(QLatin1String("@odata.nextLink")).toString());
    }
    return out;
}

void OneDriveDestination::remove(const QString &name)
{
    const HttpReply r = call("DELETE", item(name));
    if (r.status != 204 && r.status != 200 && r.status != 404)
        throw net::NetError(QStringLiteral("OneDrive: cannot remove %1 (%2)").arg(name).arg(r.status));
}

} // namespace e3::backup
