// SPDX-License-Identifier: MPL-2.0
#include "Signatures.h"

#include "Preferences.h"
#include "store/Config.h"
#include "store/Database.h"

#include <QCollator>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <algorithm>

namespace e3::mail {

namespace {

QString keyFor(const QString &id)
{
    return QLatin1String(cfg::SignaturePrefix) + id;
}

std::optional<Signature> decode(const QString &id, const QString &value)
{
    if (value.isEmpty())
        return std::nullopt; // removed
    const QJsonObject o = QJsonDocument::fromJson(value.toUtf8()).object();
    Signature s{id, o.value(QLatin1String("name")).toString(), o.value(QLatin1String("html")).toString(),
                o.value(QLatin1String("text")).toString()};
    if (s.text.trimmed().isEmpty() && s.html.isEmpty())
        return std::nullopt;
    return s;
}

QString encode(const Signature &s)
{
    return QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("name"), s.name},
                                                       {QStringLiteral("html"), s.html},
                                                       {QStringLiteral("text"), s.text}})
                                 .toJson(QJsonDocument::Compact));
}

QString defaultName()
{
    return QCoreApplication::translate("Signatures", "Signature");
}

// The signature of earlier versions, while it has not been converted.
std::optional<Signature> legacy(MailContext &ctx)
{
    if (ctx.config.isSet(keyFor(Signatures::LegacyId).toLatin1().constData()))
        return std::nullopt;
    const QString text = ctx.config.get(cfg::Signature);
    if (text.trimmed().isEmpty())
        return std::nullopt;
    return Signature{Signatures::LegacyId, defaultName(), QString(), text};
}

} // namespace

QList<Signature> Signatures::list(MailContext &ctx)
{
    QList<Signature> out;
    Statement st(ctx.db, "SELECT key, value FROM config WHERE substr(key, 1, ?) = ?");
    const QString prefix = QLatin1String(cfg::SignaturePrefix);
    st.bindAll(int(prefix.size()), prefix);
    while (st.step()) {
        if (auto s = decode(st.text(0).mid(prefix.size()), st.text(1)))
            out.append(*s);
    }
    if (auto l = legacy(ctx))
        out.append(*l);
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(out.begin(), out.end(),
                     [&](const Signature &a, const Signature &b) { return collator.compare(a.name, b.name) < 0; });
    return out;
}

std::optional<Signature> Signatures::get(MailContext &ctx, const QString &id)
{
    if (id.isEmpty())
        return std::nullopt;
    if (id == LegacyId) {
        if (auto l = legacy(ctx))
            return l;
    }
    const QByteArray key = keyFor(id).toLatin1();
    return decode(id, ctx.config.get(key.constData()));
}

QString Signatures::save(MailContext &ctx, Signature sig)
{
    if (sig.id.isEmpty())
        sig.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (sig.name.trimmed().isEmpty())
        sig.name = defaultName();
    sig.name = sig.name.trimmed();
    Transaction tx(ctx.db);
    // Until someone chooses, the default is the old signature if there is
    // one, or else the first one made; either is written down now.
    const bool chosen = ctx.config.isSet(cfg::SignatureDefault);
    const QString implicit = defaultId(ctx);
    Preferences::write(ctx, keyFor(sig.id), encode(sig));
    if (sig.id == LegacyId && !ctx.config.get(cfg::Signature).isEmpty())
        Preferences::write(ctx, QLatin1String(cfg::Signature), QString());
    if (!chosen)
        Preferences::write(ctx, QLatin1String(cfg::SignatureDefault), implicit.isEmpty() ? sig.id : implicit);
    tx.commit();
    return sig.id;
}

void Signatures::remove(MailContext &ctx, const QString &id)
{
    if (id.isEmpty())
        return;
    Transaction tx(ctx.db);
    const bool wasDefault = defaultId(ctx) == id;
    Preferences::write(ctx, keyFor(id), QString());
    if (id == LegacyId && !ctx.config.get(cfg::Signature).isEmpty())
        Preferences::write(ctx, QLatin1String(cfg::Signature), QString());
    if (wasDefault)
        Preferences::write(ctx, QLatin1String(cfg::SignatureDefault), QString());
    tx.commit();
}

QString Signatures::defaultId(MailContext &ctx)
{
    if (!ctx.config.isSet(cfg::SignatureDefault))
        return legacy(ctx) ? LegacyId : QString();
    const QString id = ctx.config.get(cfg::SignatureDefault);
    // A default removed on another device means none.
    return get(ctx, id) ? id : QString();
}

std::optional<Signature> Signatures::defaultSignature(MailContext &ctx)
{
    return get(ctx, defaultId(ctx));
}

void Signatures::setDefault(MailContext &ctx, const QString &id)
{
    Preferences::set(ctx, cfg::SignatureDefault, id);
}

} // namespace e3::mail
