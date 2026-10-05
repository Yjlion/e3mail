// SPDX-License-Identifier: MPL-2.0
#include "BlobStore.h"

#include "util/Log.h"

#include <QCryptographicHash>
#include <QDir>
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
    const QStringList names = QDir(m_dir).entryList(QDir::Files);
    for (const QString &name : names) {
        if (isValidName(name) && !referenced.contains(name) && QFile::remove(path(name)))
            ++removed;
    }
    if (removed)
        qCInfo(lcStore) << "blob gc removed" << removed;
    return removed;
}

} // namespace e3
