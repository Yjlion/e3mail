// SPDX-License-Identifier: MPL-2.0
#include "BlobStore.h"

#include "util/Log.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>

namespace e3 {

BlobStore::BlobStore(const QString &dir) : m_dir(dir)
{
    QDir().mkpath(m_dir);
}

bool BlobStore::isValidName(const QString &name)
{
    if (name.size() != 64)
        return false;
    for (QChar c : name) {
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f')))
            return false;
    }
    return true;
}

QString BlobStore::path(const QString &name) const
{
    return m_dir + u'/' + name;
}

QString BlobStore::put(const QByteArray &data)
{
    const QString name =
        QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    if (QFile::exists(path(name)))
        return name;
    QSaveFile f(path(name));
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
        throw std::runtime_error("cannot write blob " + name.toStdString() + ": "
                                 + f.errorString().toStdString());
    return name;
}

QByteArray BlobStore::get(const QString &name) const
{
    if (!isValidName(name))
        return {};
    QFile f(path(name));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

bool BlobStore::exists(const QString &name) const
{
    return isValidName(name) && QFile::exists(path(name));
}

bool BlobStore::remove(const QString &name)
{
    return isValidName(name) && QFile::remove(path(name));
}

int BlobStore::collectGarbage(const QSet<QString> &referenced)
{
    int removed = 0;
    // A blob is written before the row that references it commits, possibly
    // on another thread; anything this young may still be on its way in.
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addSecs(-3600);
    const QFileInfoList files = QDir(m_dir).entryInfoList(QDir::Files);
    for (const QFileInfo &fi : files) {
        const QString name = fi.fileName();
        if (isValidName(name) && !referenced.contains(name) && fi.lastModified().toUTC() < cutoff
            && QFile::remove(fi.absoluteFilePath()))
            ++removed;
    }
    if (removed)
        qCInfo(lcStore) << "blob gc removed" << removed;
    return removed;
}

} // namespace e3
