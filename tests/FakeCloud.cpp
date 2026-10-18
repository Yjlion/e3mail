// SPDX-License-Identifier: MPL-2.0
#include "FakeCloud.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

namespace {
QByteArray json(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
const QString kFolder = QStringLiteral("application/vnd.google-apps.folder");
constexpr int kPage = 2;
} // namespace

FakeCloud::FakeCloud()
{
    listen(QHostAddress::LocalHost);
}

void FakeCloud::incomingConnection(qintptr fd)
{
    auto *s = new QTcpSocket(this);
    s->setSocketDescriptor(fd);
    auto buf = std::make_shared<QByteArray>();
    connect(s, &QTcpSocket::readyRead, s, [this, s, buf] {
        *buf += s->readAll();
        while (true) {
            const qsizetype end = buf->indexOf("\r\n\r\n");
            if (end < 0)
                return;
            Request r;
            const QList<QByteArray> lines = buf->left(end).split('\n');
            const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
            r.verb = first.value(0);
            const QByteArray target = first.value(1);
            const qsizetype qm = target.indexOf('?');
            r.path = QUrl::fromPercentEncoding(qm < 0 ? target : target.left(qm)).toUtf8();
            r.query = qm < 0 ? QByteArray() : target.mid(qm + 1);
            for (const QByteArray &h : lines.mid(1)) {
                const qsizetype c = h.indexOf(':');
                if (c > 0)
                    r.headers.insert(h.left(c).trimmed().toLower(), h.mid(c + 1).trimmed());
            }
            const qsizetype length = r.headers.value("content-length").toLongLong();
            if (buf->size() < end + 4 + length)
                return;
            r.body = buf->mid(end + 4, length);
            buf->remove(0, end + 4 + length);
            ++requests;
            const Response out = handle(r);
            QByteArray head = "HTTP/1.1 " + QByteArray::number(out.status) + " X\r\nContent-Length: "
                              + QByteArray::number(out.body.size()) + "\r\n";
            for (const auto &[k, v] : out.headers)
                head += k + ": " + v + "\r\n";
            s->write(head + "\r\n" + out.body);
        }
    });
    connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
}

FakeCloud::Response FakeCloud::handle(const Request &r)
{
    // Graph's download and upload URLs carry their own authorization.
    if (r.path.startsWith("/download/")) {
        const QString p = QString::fromUtf8(r.path.mid(10));
        if (!graph.contains(p))
            return {404, {}, {}};
        return {200, graph.value(p), {}};
    }
    if (r.path.startsWith("/graphupload/"))
        return graphApi(r);
    const QByteArray auth = r.headers.value("authorization");
    if (!auth.startsWith("Bearer ") || !tokens.contains(QString::fromUtf8(auth.mid(7))))
        return {401, R"({"error":"unauthenticated"})", {}};
    if (r.path.startsWith("/drive/v3") || r.path.startsWith("/upload/drive/v3"))
        return driveApi(r);
    if (r.path.startsWith("/v1.0/me/drive/special/approot"))
        return graphApi(r);
    return {404, {}, {}};
}

FakeCloud::Response FakeCloud::driveApi(const Request &r)
{
    const QUrlQuery q(QString::fromUtf8(r.query));
    auto fileJson = [](const QString &id, const File &f) {
        return QJsonObject{{QStringLiteral("id"), id},
                           {QStringLiteral("name"), f.name},
                           {QStringLiteral("size"), QString::number(f.data.size())},
                           {QStringLiteral("modifiedTime"), f.modified.toString(Qt::ISODateWithMs)}};
    };
    if (r.verb == "GET" && r.path == "/drive/v3/files") {
        // name = 'x' and mimeType = '...' and 'p' in parents and trashed = false
        const QString query = q.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded);
        static const QRegularExpression nameRe(QStringLiteral("name = '((?:[^'\\\\]|\\\\.)*)'"));
        static const QRegularExpression mimeRe(QStringLiteral("mimeType = '([^']*)'"));
        static const QRegularExpression parentRe(QStringLiteral("'([^']*)' in parents"));
        const auto n = nameRe.match(query), m = mimeRe.match(query), p = parentRe.match(query);
        QString parent = p.captured(1);
        QList<QString> ids;
        for (auto it = drive.cbegin(); it != drive.cend(); ++it) {
            if (it->parent != parent)
                continue;
            if (n.hasMatch() && it->name != n.captured(1))
                continue;
            if (m.hasMatch() && it->mime != m.captured(1))
                continue;
            ids.append(it.key());
        }
        const int start = q.queryItemValue(QStringLiteral("pageToken")).toInt();
        QJsonArray files;
        for (int i = start; i < ids.size() && i < start + kPage; ++i)
            files.append(fileJson(ids[i], drive.value(ids[i])));
        QJsonObject o{{QStringLiteral("files"), files}};
        if (start + kPage < ids.size())
            o.insert(QStringLiteral("nextPageToken"), QString::number(start + kPage));
        return {200, json(o), {}};
    }
    if (r.verb == "POST" && r.path == "/drive/v3/files") {
        const QJsonObject o = QJsonDocument::fromJson(r.body).object();
        const QString id = QStringLiteral("d%1").arg(m_next++);
        drive.insert(id, {o.value(QLatin1String("name")).toString(),
                          o.value(QLatin1String("parents")).toArray().first().toString(),
                          o.value(QLatin1String("mimeType")).toString(), {}, QDateTime::currentDateTimeUtc()});
        return {200, json({{QStringLiteral("id"), id}}), {}};
    }
    if ((r.verb == "POST" && r.path == "/upload/drive/v3/files") || (r.verb == "PATCH" && r.path.startsWith("/upload/drive/v3/files/"))) {
        const QString session = QStringLiteral("s%1").arg(m_next++);
        const QString existing = r.verb == "PATCH" ? QString::fromUtf8(r.path.mid(22)) : QString();
        m_driveUploads.insert(session, {existing, QString::fromUtf8(r.body)});
        return {200, {}, {{"Location", (base() + QStringLiteral("/upload/drive/v3/session/") + session).toUtf8()}}};
    }
    if (r.verb == "PUT" && r.path.startsWith("/upload/drive/v3/session/")) {
        const auto [existing, meta] = m_driveUploads.take(QString::fromUtf8(r.path.mid(25)));
        QString id = existing;
        if (id.isEmpty()) {
            const QJsonObject o = QJsonDocument::fromJson(meta.toUtf8()).object();
            id = QStringLiteral("d%1").arg(m_next++);
            drive.insert(id, {o.value(QLatin1String("name")).toString(),
                              o.value(QLatin1String("parents")).toArray().first().toString(), QString(), {}, {}});
        }
        drive[id].data = r.body;
        drive[id].modified = QDateTime::currentDateTimeUtc();
        return {200, json({{QStringLiteral("id"), id}}), {}};
    }
    if (r.path.startsWith("/drive/v3/files/")) {
        const QString id = QString::fromUtf8(r.path.mid(16));
        if (!drive.contains(id))
            return {404, {}, {}};
        if (r.verb == "GET")
            return {200, drive.value(id).data, {}};
        if (r.verb == "DELETE") {
            drive.remove(id);
            return {204, {}, {}};
        }
    }
    return {400, {}, {}};
}

FakeCloud::Response FakeCloud::graphApi(const Request &r)
{
    if (r.path.startsWith("/graphupload/")) {
        GraphUpload &u = m_graphUploads[QString::fromUtf8(r.path.mid(13))];
        // Content-Range: bytes a-b/total
        const QByteArray range = r.headers.value("content-range");
        static const QRegularExpression re(QStringLiteral("bytes (\\d+)-(\\d+)/(\\d+)"));
        const auto m = re.match(QString::fromLatin1(range));
        if (!m.hasMatch() || m.captured(1).toLongLong() != u.data.size())
            return {416, {}, {}};
        u.total = m.captured(3).toLongLong();
        u.data += r.body;
        if (u.data.size() < u.total)
            return {202, json({{QStringLiteral("nextExpectedRanges"), QJsonArray{QString::number(u.data.size()) + u'-'}}}), {}};
        graph.insert(u.path, u.data);
        return {201, json({{QStringLiteral("name"), u.path.section(u'/', -1)}}), {}};
    }
    QString rest = QString::fromUtf8(r.path.mid(QByteArray("/v1.0/me/drive/special/approot").size()));
    if (rest == QLatin1String("/children") && r.verb == "POST") {
        const QString name = QJsonDocument::fromJson(r.body).object().value(QLatin1String("name")).toString();
        if (graph.contains(name))
            return {409, {}, {}};
        graph.insert(name, QByteArray());
        return {201, {}, {}};
    }
    // :/<path>:<suffix>
    if (!rest.startsWith(QLatin1String(":/")))
        return {400, {}, {}};
    rest = rest.mid(2);
    const qsizetype colon = rest.lastIndexOf(u':');
    const QString path = colon < 0 ? rest : rest.left(colon);
    const QString suffix = colon < 0 ? QString() : rest.mid(colon + 1);
    if (suffix == QLatin1String("/content") && r.verb == "PUT") {
        graph.insert(path, r.body);
        return {201, {}, {}};
    }
    if (suffix == QLatin1String("/content") && r.verb == "GET") {
        if (!graph.contains(path))
            return {404, {}, {}};
        return {302, {}, {{"Location", (base() + QStringLiteral("/download/") + path).toUtf8()}}};
    }
    if (suffix == QLatin1String("/createUploadSession")) {
        const QString id = QStringLiteral("u%1").arg(m_next++);
        m_graphUploads.insert(id, {path, {}, 0});
        ++uploadSessions;
        return {200, json({{QStringLiteral("uploadUrl"), base() + QStringLiteral("/graphupload/") + id}}), {}};
    }
    if (suffix == QLatin1String("/children") && r.verb == "GET") {
        QStringList names;
        for (auto it = graph.cbegin(); it != graph.cend(); ++it) {
            if (it.key().startsWith(path + u'/') && !it.key().mid(path.size() + 1).contains(u'/'))
                names.append(it.key());
        }
        const int start = QUrlQuery(QString::fromUtf8(r.query)).queryItemValue(QStringLiteral("skip")).toInt();
        QJsonArray value;
        for (int i = start; i < names.size() && i < start + kPage; ++i)
            value.append(QJsonObject{{QStringLiteral("name"), names[i].section(u'/', -1)},
                                     {QStringLiteral("size"), graph.value(names[i]).size()},
                                     {QStringLiteral("lastModifiedDateTime"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                                     {QStringLiteral("file"), QJsonObject()}});
        QJsonObject o{{QStringLiteral("value"), value}};
        if (start + kPage < names.size())
            o.insert(QStringLiteral("@odata.nextLink"),
                     base() + QStringLiteral("/v1.0/me/drive/special/approot:/%1:/children?skip=%2").arg(path).arg(start + kPage));
        return {200, json(o), {}};
    }
    if (suffix.isEmpty() && r.verb == "DELETE") {
        graph.remove(path);
        return {204, {}, {}};
    }
    return {400, {}, {}};
}
