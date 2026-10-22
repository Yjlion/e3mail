// SPDX-License-Identifier: MPL-2.0
#include "BlobStore.h"

#include "Database.h"
#include "backup/Crypto.h"
#include "util/Log.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>

namespace e3 {

namespace {
// A sealed file starts with this, then nonce, ciphertext and tag.
constexpr char kMagic[] = "E3B1";
constexpr qsizetype kMagicSize = 4;
constexpr qsizetype kSealOverhead = 24 + 16;

QString sha256Hex(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeFile(const QString &path, const QByteArray &data, const QString &name)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
        throw std::runtime_error("cannot write blob " + name.toStdString() + ": " + f.errorString().toStdString());
}
} // namespace

BlobStore::BlobStore(const QString &dir, const QByteArray &dbKey) : m_dir(dir)
{
    QDir().mkpath(m_dir);
    if (!dbKey.isEmpty())
        m_key = backup::crypto::hmac(dbKey, "e3mail blobs v1");
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

QString BlobStore::clearPath(const QString &name) const
{
    return m_dir + u'/' + name;
}

QString BlobStore::sealedPath(const QString &name) const
{
    return m_dir + u'/' + QString::fromLatin1(backup::crypto::hmac(m_key, name.toLatin1()).toHex());
}

QString BlobStore::path(const QString &name) const
{
    if (m_key.isEmpty())
        return clearPath(name);
    const QString sealed = sealedPath(name);
    return QFile::exists(sealed) || !QFile::exists(clearPath(name)) ? sealed : clearPath(name);
}

QString BlobStore::put(const QByteArray &data)
{
    const QString name = sha256Hex(data);
    if (m_key.isEmpty()) {
        if (!QFile::exists(clearPath(name)))
            writeFile(clearPath(name), data, name);
        return name;
    }
    const QString sealed = sealedPath(name);
    if (!QFile::exists(sealed))
        writeFile(sealed, kMagic + backup::crypto::seal(m_key, data, name.toLatin1()), name);
    return name;
}

QByteArray BlobStore::get(const QString &name) const
{
    if (!isValidName(name))
        return {};
    if (!m_key.isEmpty()) {
        const QByteArray file = readFile(sealedPath(name));
        if (file.startsWith(kMagic)) {
            const auto plain = backup::crypto::open(m_key, file.mid(kMagicSize), name.toLatin1());
            if (!plain)
                qCWarning(lcStore) << "blob" << name.left(12) << "does not open: damaged or not ours";
            return plain.value_or(QByteArray());
        }
    }
    // In the clear, from before encryption at rest: still checked against
    // its name.
    const QByteArray data = readFile(clearPath(name));
    return !data.isNull() && sha256Hex(data) == name ? data : QByteArray();
}

bool BlobStore::exists(const QString &name) const
{
    return isValidName(name) && (QFile::exists(clearPath(name)) || (!m_key.isEmpty() && QFile::exists(sealedPath(name))));
}

qint64 BlobStore::size(const QString &name) const
{
    if (!isValidName(name))
        return -1;
    if (!m_key.isEmpty()) {
        const QFileInfo sealed(sealedPath(name));
        if (sealed.exists())
            return qMax<qint64>(0, sealed.size() - kMagicSize - kSealOverhead);
    }
    const QFileInfo clear(clearPath(name));
    return clear.exists() ? clear.size() : -1;
}

bool BlobStore::remove(const QString &name)
{
    if (!isValidName(name))
        return false;
    const bool clear = QFile::remove(clearPath(name));
    const bool sealed = !m_key.isEmpty() && QFile::remove(sealedPath(name));
    return clear || sealed;
}

int BlobStore::collectGarbage(const QSet<QString> &referenced)
{
    // Files are named by content, or by an HMAC of it: keep both forms of
    // every referenced name.
    QSet<QString> keep = referenced;
    if (!m_key.isEmpty()) {
        for (const QString &n : referenced)
            keep.insert(QFileInfo(sealedPath(n)).fileName());
    }
    int removed = 0;
    // A blob is written before the row that references it commits, possibly
    // on another thread; anything this young may still be on its way in.
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addSecs(-3600);
    const QFileInfoList files = QDir(m_dir).entryInfoList(QDir::Files);
    for (const QFileInfo &fi : files) {
        const QString name = fi.fileName();
        if (isValidName(name) && !keep.contains(name) && fi.lastModified().toUTC() < cutoff
            && QFile::remove(fi.absoluteFilePath()))
            ++removed;
    }
    if (removed)
        qCInfo(lcStore) << "blob gc removed" << removed;
    return removed;
}

int BlobStore::encryptLegacy()
{
    if (m_key.isEmpty())
        return 0;
    int sealed = 0;
    const QFileInfoList files = QDir(m_dir).entryInfoList(QDir::Files);
    for (const QFileInfo &fi : files) {
        const QString name = fi.fileName();
        if (!isValidName(name))
            continue;
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly) || f.peek(kMagicSize) == kMagic)
            continue; // ours already
        const QByteArray data = f.readAll();
        f.close();
        if (sha256Hex(data) != name)
            continue; // damaged, or not a blob: left as it is
        put(data);
        QFile::remove(fi.absoluteFilePath());
        ++sealed;
    }
    if (sealed)
        qCInfo(lcStore) << "sealed" << sealed << "blobs written in the clear by an older version";
    return sealed;
}

QStringList BlobStore::referenced(Database &db)
{
    QStringList out;
    Statement st(db, "SELECT raw_blob FROM messages WHERE raw_blob IS NOT NULL UNION SELECT blob FROM attachments");
    while (st.step())
        out.append(st.text(0));
    return out;
}

} // namespace e3
