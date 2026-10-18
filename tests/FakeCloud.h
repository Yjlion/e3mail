// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QDateTime>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QTcpServer>

// Google Drive (files, resumable uploads) and Microsoft Graph (the app's
// folder, upload sessions, downloads served elsewhere) over plain HTTP on
// 127.0.0.1, in memory, enough for e3mail's backup destinations (ADR 0020).
// Both page their listings two at a time. Requests need a token in `tokens`.
class FakeCloud : public QTcpServer
{
    Q_OBJECT
public:
    FakeCloud();
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()); }
    QSet<QString> tokens;
    int uploadSessions = 0; // Graph's, for large files
    int requests = 0;

    struct File
    {
        QString name, parent, mime;
        QByteArray data;
        QDateTime modified;
    };
    QMap<QString, File> drive;      // id -> file
    QMap<QString, QByteArray> graph; // path under the app folder -> content ("" for a folder)

protected:
    void incomingConnection(qintptr fd) override;

private:
    struct Request
    {
        QByteArray verb, path, query, body;
        QHash<QByteArray, QByteArray> headers;
    };
    struct Response
    {
        int status = 200;
        QByteArray body;
        QList<QPair<QByteArray, QByteArray>> headers;
    };
    Response handle(const Request &r);
    Response driveApi(const Request &r);
    Response graphApi(const Request &r);

    int m_next = 1;
    QHash<QString, QPair<QString, QString>> m_driveUploads; // session -> (file id or "", parent|name JSON)
    struct GraphUpload
    {
        QString path;
        QByteArray data;
        qint64 total = 0;
    };
    QHash<QString, GraphUpload> m_graphUploads;
};
