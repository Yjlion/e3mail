// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Destination.h"
#include "Http.h"

namespace e3::backup {

// A WebDAV folder (Nextcloud, ownCloud, a NAS, rclone serve webdav): PUT,
// GET, PROPFIND, DELETE and MKCOL, with Basic authentication over HTTPS.
class WebDavDestination : public Destination
{
public:
    WebDavDestination(const QUrl &base, const QString &user, const QString &password, const QByteArray &pin = {});
    void prepare() override;
    void put(const QString &name, const QByteArray &data) override;
    std::optional<QByteArray> get(const QString &name) override;
    QList<Entry> list(const QString &dir) override;
    void remove(const QString &name) override;

    // For tests: the multistatus answer of a PROPFIND, as entries.
    static QList<Entry> parseMultistatus(const QByteArray &xml);

private:
    QUrl at(const QString &name) const;
    HttpReply call(const QByteArray &verb, const QString &name, const QByteArray &body = {},
                   const QList<QPair<QByteArray, QByteArray>> &extra = {});

    QUrl m_base;
    QByteArray m_auth;
    Http m_http;
};

} // namespace e3::backup
