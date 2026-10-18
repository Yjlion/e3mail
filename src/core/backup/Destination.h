// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

#include <memory>
#include <optional>

// Where backups go (ADR 0020): a store of named objects. Names are
// "key", "objects/<hex>" and "snapshots/<name>". Calls block the calling
// thread (linear code over a local event loop, ADR 0011) and throw
// net::NetError when the destination cannot be reached or refuses.
namespace e3::backup {

struct Entry
{
    QString name;      // the last part: "<hex>" in "objects/<hex>"
    qint64 size = 0;
    QDateTime modified; // invalid if the destination does not say
};

class Destination
{
public:
    virtual ~Destination() = default;
    // Creates whatever the destination needs ("objects/", "snapshots/").
    virtual void prepare() {}
    virtual void put(const QString &name, const QByteArray &data) = 0;
    // Nothing when there is no such object.
    virtual std::optional<QByteArray> get(const QString &name) = 0;
    // The objects directly under "objects" or "snapshots".
    virtual QList<Entry> list(const QString &dir) = 0;
    virtual void remove(const QString &name) = 0;
};

// A folder: a USB drive, a folder another program syncs, and the tests.
class FolderDestination : public Destination
{
public:
    explicit FolderDestination(const QString &root) : m_root(root) {}
    void prepare() override;
    void put(const QString &name, const QByteArray &data) override;
    std::optional<QByteArray> get(const QString &name) override;
    QList<Entry> list(const QString &dir) override;
    void remove(const QString &name) override;

private:
    QString m_root;
};

} // namespace e3::backup
