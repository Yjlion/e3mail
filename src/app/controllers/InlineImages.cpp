// SPDX-License-Identifier: MPL-2.0
#include "InlineImages.h"

#include <QBuffer>
#include <QHash>
#include <QImageReader>
#include <QMutex>

namespace {

constexpr int kKeptMessages = 20;

struct Store
{
    QMutex lock;
    QHash<QString, QImage> images; // "<message>-<index>"
    QStringList messages;          // most recent last
};

Store &store()
{
    static Store s;
    return s;
}

} // namespace

QString InlineImages::put(const QString &message, int index, const QImage &image)
{
    Store &s = store();
    const QMutexLocker locker(&s.lock);
    s.messages.removeAll(message);
    s.messages.append(message);
    while (s.messages.size() > kKeptMessages) {
        const QString old = s.messages.takeFirst() + u'-';
        s.images.removeIf([&old](const auto &it) { return it.key().startsWith(old); });
    }
    const QString key = message + u'-' + QString::number(index);
    s.images.insert(key, image);
    return QStringLiteral("image://cid/") + key;
}

QImage InlineImages::requestImage(const QString &id, QSize *size, const QSize &)
{
    Store &s = store();
    const QMutexLocker locker(&s.lock);
    const QImage img = s.images.value(id);
    if (size)
        *size = img.size();
    return img;
}

QImage InlineImages::decode(const QByteArray &data, const QString &mimeType)
{
    static const QStringList formats = {QStringLiteral("image/png"), QStringLiteral("image/jpeg"),
                                        QStringLiteral("image/jpg"), QStringLiteral("image/gif"),
                                        QStringLiteral("image/webp")};
    if (data.size() > kMaxBytes || !formats.contains(mimeType.toLower()))
        return {};
    QBuffer buf;
    buf.setData(data);
    QImageReader reader(&buf);
    // A small file can claim an enormous image; refuse rather than allocate.
    const QSize claimed = reader.size();
    if (!claimed.isValid() || qint64(claimed.width()) * claimed.height() > 40'000'000)
        return {};
    QImage img = reader.read();
    if (img.isNull())
        return {};
    if (img.width() > kMaxWidth)
        img = img.scaledToWidth(kMaxWidth, Qt::SmoothTransformation);
    return img;
}
