// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Destination.h"
#include "Http.h"

#include <QDateTime>

namespace e3::backup {

// An S3 bucket or one of its compatibles (Backblaze B2, MinIO, Wasabi,
// Cloudflare R2), signed with AWS Signature Version 4.
class S3Destination : public Destination
{
public:
    struct Options
    {
        QUrl endpoint;    // https://s3.eu-central-1.amazonaws.com, http://127.0.0.1:9000
        QString region;   // us-east-1 where the service does not care
        QString bucket;
        QString prefix;   // a folder inside the bucket; may be empty
        QString accessKey;
        QString secretKey;
        bool pathStyle = true; // bucket in the path, not in the host name
        QByteArray pin;
    };
    explicit S3Destination(const Options &o);
    void prepare() override;
    void put(const QString &name, const QByteArray &data) override;
    std::optional<QByteArray> get(const QString &name) override;
    QList<Entry> list(const QString &dir) override;
    void remove(const QString &name) override;

    // The Authorization header for a request, from what SigV4 signs. Public
    // for AWS's published test vectors.
    static QByteArray authorization(const QByteArray &verb, const QUrl &url,
                                    QList<QPair<QByteArray, QByteArray>> headers, const QByteArray &payloadSha256,
                                    const QDateTime &when, const QString &region, const QString &service,
                                    const QString &accessKey, const QString &secretKey);

private:
    QUrl urlFor(const QString &key) const;
    HttpReply call(const QByteArray &verb, const QUrl &url, const QByteArray &body = {});

    Options m_o;
    Http m_http;
};

} // namespace e3::backup
