// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "util/Hlc.h"

#include <QJsonObject>
#include <QString>

namespace e3 {

class Database;

// One entry of the op-log, as stored and as sent to other devices.
struct Op
{
    QString device;
    qint64 seq = 0;
    qint64 hlc = 0;
    QString kind;
    QJsonObject payload;
};

// Records every user-visible change as an operation, so another device of the
// same user can replay it. With one device it is only a log; the point of
// writing it from the first schema is that multi-client sync is not
// retrofitted onto a store that never said what changed.
//
// Ops name messages by Message-ID, never by local row id, which differs
// between devices.
class OpLog
{
public:
    explicit OpLog(Database &db);

    // Must be called inside the caller's transaction, so the op and the change
    // it describes commit together.
    qint64 record(const QString &kind, const QJsonObject &payload);

    QString deviceId() const { return m_device; }

    // What an op competes with: of all ops with the same key, the one with the
    // highest (hlc, device) wins. Empty for ops that only ever add. Computed
    // from kind and payload on every device, never taken from the wire
    // (ADR 0013).
    static QString mergeKey(const QString &kind, const QJsonObject &payload);

private:
    Database &m_db;
    QString m_device;
    Hlc m_clock;
};

namespace op {
inline const QString MessageAdded = QStringLiteral("msg.add");
inline const QString ReadState = QStringLiteral("msg.read");
inline const QString LabelAdd = QStringLiteral("label.add");
inline const QString LabelRemove = QStringLiteral("label.remove");
inline const QString LabelDefine = QStringLiteral("label.define");
inline const QString LabelRename = QStringLiteral("label.rename");
inline const QString LabelDelete = QStringLiteral("label.delete");
inline const QString Trash = QStringLiteral("msg.trash");
inline const QString Restore = QStringLiteral("msg.restore");
inline const QString Purge = QStringLiteral("msg.purge");
inline const QString Accept = QStringLiteral("contact.accept");
inline const QString Block = QStringLiteral("block.add");
inline const QString Unblock = QStringLiteral("block.remove");
inline const QString ContactEdit = QStringLiteral("contact.edit");
inline const QString ContactPolicy = QStringLiteral("contact.policy");
inline const QString ContactVerify = QStringLiteral("contact.verify");
inline const QString ContactDetails = QStringLiteral("contact.details");
inline const QString ContactRemove = QStringLiteral("contact.remove");
inline const QString KeyLearned = QStringLiteral("key.learned");
inline const QString ServerAck = QStringLiteral("server.ack");
inline const QString Setting = QStringLiteral("config.set");
} // namespace op

} // namespace e3
