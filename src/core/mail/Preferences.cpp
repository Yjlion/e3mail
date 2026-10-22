// SPDX-License-Identifier: MPL-2.0
#include "Preferences.h"

#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"

#include <QJsonObject>

namespace e3::mail {

bool Preferences::isSynced(const QString &key)
{
    static const QStringList synced = {
        QLatin1String(cfg::DisplayName),     QLatin1String(cfg::Signature),
        QLatin1String(cfg::EncryptionMode),  QLatin1String(cfg::ServerRetention),
        QLatin1String(cfg::ServerKeepDays),  QLatin1String(cfg::TrashPurgeDays),
        QLatin1String(cfg::UnverifiedTrashDays), QLatin1String(cfg::Gating),
        QLatin1String(cfg::SignatureDefault), QLatin1String(cfg::MdnSend),
    };
    // Each signature is a setting of its own (Signatures), so two devices
    // editing different ones both keep their edit.
    return synced.contains(key) || key.startsWith(QLatin1String(cfg::SignaturePrefix));
}

void Preferences::set(MailContext &ctx, const char *key, const QString &value)
{
    Transaction tx(ctx.db);
    write(ctx, QString::fromLatin1(key), value);
    tx.commit();
}

void Preferences::write(MailContext &ctx, const QString &key, const QString &value)
{
    apply(ctx, key, value);
    if (isSynced(key))
        ctx.ops.record(op::Setting, {{QStringLiteral("key"), key}, {QStringLiteral("value"), value}});
}

void Preferences::apply(MailContext &ctx, const QString &key, const QString &value)
{
    const QByteArray k = key.toLatin1();
    ctx.config.set(k.constData(), value);
    // Turning gating off releases everything waiting.
    if (key == QLatin1String(cfg::Gating) && value.toInt() == 0)
        ctx.db.run("DELETE FROM held");
}

} // namespace e3::mail
