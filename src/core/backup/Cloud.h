// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Destination.h"
#include "Http.h"
#include "net/OAuth.h"

#include <QHash>

namespace e3::backup {

// Google Drive: a folder "e3mail backup" in the person's Drive, which the
// person can see; e3mail asks only for the files it creates (drive.file).
class DriveDestination : public Destination
{
public:
    // `api`: Google's, or a fake's in tests.
    DriveDestination(std::shared_ptr<net::oauth::TokenSource> tokens, const QString &folder = QStringLiteral("e3mail backup"),
                     const QUrl &api = QUrl(QStringLiteral("https://www.googleapis.com")));
    void prepare() override;
    void put(const QString &name, const QByteArray &data) override;
    std::optional<QByteArray> get(const QString &name) override;
    QList<Entry> list(const QString &dir) override;
    void remove(const QString &name) override;

private:
    HttpReply call(const QByteArray &verb, const QUrl &url, const QByteArray &body = {},
                   const QList<QPair<QByteArray, QByteArray>> &extra = {});
    QString folderId(const QString &name, const QString &parent, bool create);
    QString parentOf(const QString &name);
    QString idOf(const QString &name);
    QUrl url(const QString &path, const QList<QPair<QString, QString>> &query = {}) const;

    std::shared_ptr<net::oauth::TokenSource> m_tokens;
    QString m_folder;
    QUrl m_api;
    Http m_http;
    QHash<QString, QString> m_dirs; // "" | "objects" | "snapshots" -> folder id
    QHash<QString, QString> m_ids;  // object name -> file id, as listed
};

// OneDrive: the app's own folder (Apps/e3mail), through Microsoft Graph.
class OneDriveDestination : public Destination
{
public:
    OneDriveDestination(std::shared_ptr<net::oauth::TokenSource> tokens,
                        const QUrl &api = QUrl(QStringLiteral("https://graph.microsoft.com/v1.0")));
    void prepare() override;
    void put(const QString &name, const QByteArray &data) override;
    std::optional<QByteArray> get(const QString &name) override;
    QList<Entry> list(const QString &dir) override;
    void remove(const QString &name) override;

private:
    HttpReply call(const QByteArray &verb, const QUrl &url, const QByteArray &body = {},
                   const QList<QPair<QByteArray, QByteArray>> &extra = {}, bool auth = true);
    QUrl item(const QString &path, const QString &suffix = {}) const;

    std::shared_ptr<net::oauth::TokenSource> m_tokens;
    QUrl m_api;
    Http m_http;
};

} // namespace e3::backup
