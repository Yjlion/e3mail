// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QStringList>

namespace e3 {

class Database;

// Content-addressed file store for attachments and retained originals. A blob
// is named by the SHA-256 of its content, so storing the same bytes twice
// costs nothing and two devices name the same attachment the same way.
//
// With a key (ADR 0023), each file is sealed (XChaCha20-Poly1305, bound to
// its name) and stored under an HMAC of the name, so the disk shows neither
// content nor which known file is present. Files an older version wrote in
// the clear are still read, and `encryptLegacy` seals them.
class BlobStore
{
public:
    // `dbKey` is the account's database key; the blob key is derived from it.
    // Empty: files are stored in the clear (tests of the old layout only).
    explicit BlobStore(const QString &dir, const QByteArray &dbKey = {});

    QString put(const QByteArray &data);
    // The content, checked; empty if missing or damaged.
    QByteArray get(const QString &name) const;
    bool exists(const QString &name) const;
    // The size of the content, or -1.
    qint64 size(const QString &name) const;
    bool remove(const QString &name);
    // Where the file for `name` is, sealed or not.
    QString path(const QString &name) const;

    // Removes every blob not in `referenced`. Returns the number removed.
    int collectGarbage(const QSet<QString> &referenced);
    // Seals the files an older version left in the clear. Resumable: each
    // file is sealed, then the clear one removed. Returns how many.
    int encryptLegacy();

    // Every blob the database refers to, by name: what is worth keeping,
    // copying to a new device or backing up.
    static QStringList referenced(Database &db);

    QString dir() const { return m_dir; }

private:
    static bool isValidName(const QString &name);
    QString sealedPath(const QString &name) const;
    QString clearPath(const QString &name) const;
    QString m_dir;
    QByteArray m_key; // empty: no sealing
};

} // namespace e3
