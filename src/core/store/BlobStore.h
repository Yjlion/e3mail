// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QSet>
#include <QString>

namespace e3 {

// Content-addressed file store for attachments and retained originals. A blob
// is named by the SHA-256 of its content, so storing the same bytes twice
// costs nothing and two devices name the same attachment the same way.
class BlobStore
{
public:
    explicit BlobStore(const QString &dir);

    QString put(const QByteArray &data);
    QByteArray get(const QString &name) const;
    bool exists(const QString &name) const;
    bool remove(const QString &name);
    QString path(const QString &name) const;

    // Removes every blob not in `referenced`. Returns the number removed.
    int collectGarbage(const QSet<QString> &referenced);

    QString dir() const { return m_dir; }

private:
    static bool isValidName(const QString &name);
    QString m_dir;
};

} // namespace e3
