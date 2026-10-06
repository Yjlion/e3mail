// SPDX-License-Identifier: MPL-2.0
#include "OpLog.h"

#include "Config.h"
#include "Database.h"

#include <QJsonDocument>
#include <QUuid>

namespace e3 {

OpLog::OpLog(Database &db) : m_db(db)
{
    Config config(db);
    m_device = config.get(cfg::DeviceId);
    if (m_device.isEmpty()) {
        m_device = QUuid::createUuid().toString(QUuid::WithoutBraces);
        config.set(cfg::DeviceId, m_device);
    }
    m_clock = Hlc(db.queryInt("SELECT max(hlc) FROM ops").value_or(0));
}

qint64 OpLog::record(const QString &kind, const QJsonObject &payload)
{
    const qint64 seq =
        m_db.queryInt("SELECT coalesce(max(seq), 0) + 1 FROM ops WHERE device=?", m_device).value_or(1);
    const QString json = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    m_db.run("INSERT INTO ops(device, seq, hlc, kind, payload) VALUES(?, ?, ?, ?, ?)",
             m_device, seq, m_clock.now(), kind, json);
    return seq;
}

} // namespace e3
