// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "util/Hlc.h"

#include <QJsonObject>
#include <QString>

namespace e3 {

class Database;

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
inline const QString LabelDelete = QStringLiteral("label.delete");
inline const QString Trash = QStringLiteral("msg.trash");
inline const QString Restore = QStringLiteral("msg.restore");
inline const QString Purge = QStringLiteral("msg.purge");
inline const QString Accept = QStringLiteral("contact.accept");
inline const QString Block = QStringLiteral("block.add");
inline const QString Unblock = QStringLiteral("block.remove");
inline const QString ContactEdit = QStringLiteral("contact.edit");
inline const QString KeyLearned = QStringLiteral("key.learned");
inline const QString ServerAck = QStringLiteral("server.ack");
inline const QString Setting = QStringLiteral("config.set");
} // namespace op

} // namespace e3
