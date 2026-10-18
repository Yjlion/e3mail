// SPDX-License-Identifier: MPL-2.0
#include "Remote.h"
#include "Cloud.h"
#include "S3.h"
#include "WebDav.h"
#ifdef E3MAIL_HAVE_SFTP
#include "Sftp.h"
#endif

namespace e3::backup {

namespace {
QString str(const QJsonObject &c, const char *key)
{
    return c.value(QLatin1String(key)).toString();
}

// The sign-in a cloud destination was set up with: the client it was issued
// to, and where it is renewed.
std::shared_ptr<net::oauth::TokenSource> tokens(const QJsonObject &c, const QString &provider, const QString &refresh)
{
    net::oauth::Provider p = net::oauth::known(provider).value_or(net::oauth::Provider());
    if (!str(c, "clientId").isEmpty()) {
        p.clientId = str(c, "clientId");
        p.clientSecret = str(c, "clientSecret");
    }
    if (!str(c, "tokenUrl").isEmpty())
        p.tokenUrl = QUrl(str(c, "tokenUrl"));
    return std::make_shared<net::oauth::TokenSource>(p, refresh);
}
} // namespace

std::unique_ptr<Destination> remoteDestination(const QJsonObject &c, const QString &secret)
{
    const QString type = str(c, "type");
    if (type == QLatin1String("webdav"))
        return std::make_unique<WebDavDestination>(QUrl(str(c, "url")), str(c, "user"), secret, str(c, "pin").toLatin1());
    if (type == QLatin1String("s3")) {
        S3Destination::Options o;
        o.endpoint = QUrl(str(c, "endpoint"));
        o.region = str(c, "region").isEmpty() ? QStringLiteral("us-east-1") : str(c, "region");
        o.bucket = str(c, "bucket");
        o.prefix = str(c, "prefix");
        o.accessKey = str(c, "accessKey");
        o.secretKey = secret;
        o.pathStyle = c.value(QLatin1String("pathStyle")).toBool(true);
        o.pin = str(c, "pin").toLatin1();
        return std::make_unique<S3Destination>(o);
    }
#ifdef E3MAIL_HAVE_SFTP
    if (type == QLatin1String("sftp")) {
        SftpDestination::Options o;
        o.host = str(c, "host");
        o.port = quint16(c.value(QLatin1String("port")).toInt(22));
        o.user = str(c, "user");
        o.path = str(c, "path");
        o.keyFile = str(c, "keyFile");
        o.hostKey = str(c, "hostKey").toLatin1();
        o.password = secret;
        return std::make_unique<SftpDestination>(o);
    }
#endif
    if (type == QLatin1String("gdrive"))
        return std::make_unique<DriveDestination>(
            tokens(c, QStringLiteral("google"), secret),
            str(c, "folder").isEmpty() ? QStringLiteral("e3mail backup") : str(c, "folder"),
            QUrl(str(c, "api").isEmpty() ? QStringLiteral("https://www.googleapis.com") : str(c, "api")));
    if (type == QLatin1String("onedrive"))
        return std::make_unique<OneDriveDestination>(
            tokens(c, QStringLiteral("microsoft"), secret),
            QUrl(str(c, "api").isEmpty() ? QStringLiteral("https://graph.microsoft.com/v1.0") : str(c, "api")));
    return nullptr;
}

} // namespace e3::backup
