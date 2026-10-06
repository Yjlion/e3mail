// SPDX-License-Identifier: MPL-2.0
#include "Paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace e3 {

namespace {
QString s_override;

QString appDir()
{
    return QCoreApplication::instance() ? QCoreApplication::applicationDirPath() : QString();
}
} // namespace

bool Paths::isPortable()
{
    const QString dir = appDir();
    if (dir.isEmpty())
        return false;
    return QFileInfo::exists(dir + QStringLiteral("/portable.txt"))
        || QFileInfo(dir + QStringLiteral("/data")).isDir();
}

QString Paths::dataDir()
{
    QString dir;
    if (!s_override.isEmpty())
        dir = s_override;
    else if (const QByteArray env = qgetenv("E3MAIL_DATA_DIR"); !env.isEmpty())
        dir = QString::fromLocal8Bit(env);
    else if (isPortable())
        dir = appDir() + QStringLiteral("/data");
    else
        dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    QDir().mkpath(dir);
    return QDir(dir).absolutePath();
}

void Paths::setDataDir(const QString &dir)
{
    s_override = dir;
}

QString Paths::firstRunMarker()
{
    return dataDir() + QStringLiteral("/first-run-acknowledged");
}

} // namespace e3
