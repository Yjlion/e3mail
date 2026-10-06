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
    // Past every op in the log, including those another connection wrote or
    // another device sent, so a local change always outranks what it saw.
    const qint64 hlc = m_clock.observe(m_db.queryInt("SELECT max(hlc) FROM ops").value_or(0));
    const QString json = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    // The sequence number is taken in the insert itself: the UI and the
    // worker record on separate connections, often outside a transaction.
    m_db.run("INSERT INTO ops(device, seq, hlc, kind, payload, mkey) "
             "SELECT ?1, coalesce(max(seq), 0) + 1, ?2, ?3, ?4, ?5 FROM ops WHERE device=?1",
             m_device, hlc, kind, json, nullIfEmpty(mergeKey(kind, payload)));
    return m_db.queryInt("SELECT seq FROM ops WHERE id=?", m_db.lastInsertId()).value_or(0);
}

QString OpLog::mergeKey(const QString &kind, const QJsonObject &p)
{
    auto s = [&p](const char *field) { return p.value(QLatin1String(field)).toString(); };
    if (kind == op::ReadState)
        return QStringLiteral("read:") + s("mid");
    if (kind == op::Trash || kind == op::Restore)
        return QStringLiteral("trash:") + s("mid");
    if (kind == op::LabelAdd || kind == op::LabelRemove)
        return QStringLiteral("label:%1:%2").arg(s("mid"), s("label").toLower());
    if (kind == op::LabelDefine || kind == op::LabelRename || kind == op::LabelDelete)
        return QStringLiteral("labeldef:") + s("name").toLower();
    if (kind == op::Block || kind == op::Unblock)
        return QStringLiteral("block:") + s("pattern");
    if (kind == op::ContactEdit)
        return QStringLiteral("cname:") + s("addr");
    if (kind == op::ContactPolicy)
        return QStringLiteral("cpolicy:") + s("addr");
    if (kind == op::ContactVerify)
        return QStringLiteral("cverify:") + s("addr");
    if (kind == op::Setting)
        return QStringLiteral("cfg:") + s("key");
    return {};
}

} // namespace e3
