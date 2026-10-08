// SPDX-License-Identifier: MPL-2.0
#include "FileIo.h"

#include <QFile>
#include <QSaveFile>

namespace FileIo {

bool write(const QUrl &url, const QByteArray &data)
{
    if (url.isLocalFile()) {
        QSaveFile f(url.toLocalFile());
        return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit();
    }
    QFile f(url.toString());
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

std::optional<QByteArray> read(const QUrl &url)
{
    QFile f(url.isLocalFile() ? url.toLocalFile() : url.toString());
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    return f.readAll();
}

QString displayName(const QUrl &url)
{
    const QString name = url.fileName();
    return name.isEmpty() ? url.toDisplayString() : name;
}

} // namespace FileIo
