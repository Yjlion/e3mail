// SPDX-License-Identifier: MPL-2.0
#include "Destination.h"
#include "net/SyncSocket.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace e3::backup {

void FolderDestination::prepare()
{
    for (const char *d : {"objects", "snapshots"}) {
        if (!QDir().mkpath(m_root + u'/' + QLatin1String(d)))
            throw net::NetError(QStringLiteral("cannot create %1").arg(m_root));
    }
}

void FolderDestination::put(const QString &name, const QByteArray &data)
{
    QSaveFile f(m_root + u'/' + name);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
        throw net::NetError(QStringLiteral("cannot write %1: %2").arg(name, f.errorString()));
}

std::optional<QByteArray> FolderDestination::get(const QString &name)
{
    QFile f(m_root + u'/' + name);
    if (!f.exists())
        return std::nullopt;
    if (!f.open(QIODevice::ReadOnly))
        throw net::NetError(QStringLiteral("cannot read %1: %2").arg(name, f.errorString()));
    return f.readAll();
}

QList<Entry> FolderDestination::list(const QString &dir)
{
    QList<Entry> out;
    for (const QFileInfo &fi : QDir(m_root + u'/' + dir).entryInfoList(QDir::Files))
        out.append({fi.fileName(), fi.size(), fi.lastModified().toUTC()});
    return out;
}

void FolderDestination::remove(const QString &name)
{
    QFile::remove(m_root + u'/' + name);
}

} // namespace e3::backup
