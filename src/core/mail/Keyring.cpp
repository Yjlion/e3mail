// SPDX-License-Identifier: MPL-2.0
#include "Keyring.h"

#include "crypto/Pgp.h"
#include "store/Config.h"
#include "store/Database.h"
#include "util/Log.h"

#include <QDateTime>

namespace e3::mail {

void Keyring::loadAll(MailContext &ctx)
{
    Statement st(ctx.db, "SELECT data FROM keys ORDER BY secret DESC");
    while (st.step()) {
        try {
            ctx.pgp.import(st.blob(0));
        } catch (const crypto::PgpError &e) {
            qCWarning(lcCrypto) << "skipping unreadable stored key:" << e.what();
        }
    }
}

bool Keyring::ensure(MailContext &ctx, const QString &fpr)
{
    if (fpr.isEmpty())
        return false;
    if (ctx.pgp.hasKey(fpr))
        return true;
    Statement st(ctx.db, "SELECT data FROM keys WHERE fingerprint=?");
    st.bind(1, fpr);
    if (!st.step())
        return false;
    ctx.pgp.import(st.blob(0));
    return ctx.pgp.hasKey(fpr);
}

QStringList Keyring::store(MailContext &ctx, const QByteArray &keydata, bool secret)
{
    const QStringList fprs = ctx.pgp.import(keydata);
    for (const QString &fpr : fprs) {
        // Store our own serialisation, merged with anything already known.
        const QByteArray data = secret ? ctx.pgp.exportSecret(fpr) : ctx.pgp.exportPublic(fpr);
        ctx.db.run("INSERT INTO keys(fingerprint, data, secret, created_at) VALUES(?, ?, ?, ?) "
                   "ON CONFLICT(fingerprint) DO UPDATE SET data=excluded.data, "
                   "secret=max(secret, excluded.secret)",
                   fpr, data, secret, QDateTime::currentMSecsSinceEpoch());
    }
    return fprs;
}

QString Keyring::selfFingerprint(MailContext &ctx)
{
    return ctx.config.get(cfg::SelfFingerprint);
}

QString Keyring::ensureSelfKey(MailContext &ctx, const QString &addr)
{
    QString fpr = selfFingerprint(ctx);
    if (!fpr.isEmpty() && ensure(ctx, fpr))
        return fpr;
    fpr = ctx.pgp.generate(QStringLiteral("<%1>").arg(addr));
    ctx.db.run("INSERT OR REPLACE INTO keys(fingerprint, data, secret, created_at) VALUES(?, ?, 1, ?)", fpr,
               ctx.pgp.exportSecret(fpr), QDateTime::currentMSecsSinceEpoch());
    ctx.config.set(cfg::SelfFingerprint, fpr);
    qCInfo(lcCrypto) << "generated key" << fpr << "for" << addr;
    return fpr;
}

} // namespace e3::mail
