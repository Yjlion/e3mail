// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Types.h"

#include <optional>

namespace e3::mail {

struct Signature
{
    QString id;
    QString name;
    QString html; // the composer's whitelisted HTML; empty when unformatted
    QString text; // the plain-text form, always present
};

// Named signatures, one of them (or none) the default for new mail. Each is
// a synced setting of its own, `signature:<id>`, so two devices editing
// different signatures both keep their edit (ADR 0013, amended 2026-10-10).
//
// The one plain-text signature of earlier versions reads as the signature
// with id "legacy" until it is edited or removed. The id is fixed, so two
// devices converting it agree.
class Signatures
{
public:
    static inline const QString LegacyId = QStringLiteral("legacy");

    // Sorted by name.
    static QList<Signature> list(MailContext &ctx);
    static std::optional<Signature> get(MailContext &ctx, const QString &id);
    // Creates (empty id) or replaces one, and returns its id. The first
    // signature becomes the default unless a choice was made before.
    static QString save(MailContext &ctx, Signature sig);
    static void remove(MailContext &ctx, const QString &id);

    // Empty when new mail gets no signature.
    static QString defaultId(MailContext &ctx);
    static std::optional<Signature> defaultSignature(MailContext &ctx);
    static void setDefault(MailContext &ctx, const QString &id);
};

} // namespace e3::mail
