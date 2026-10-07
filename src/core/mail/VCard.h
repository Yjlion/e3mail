// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Types.h"

namespace e3::mail {

// vCard files, for the address book's import and export. Reads 2.1, 3.0 and
// 4.0; writes 4.0. Keys (KEY) are neither read nor written: e3mail learns
// keys from Autocrypt and verification only, never from a file someone hands
// over (ADR 0013 amendment).
class VCard
{
public:
    struct Card
    {
        QString name;
        QStringList emails; // normalised, in file order
        QString organization;
        QString title;
        QString notes;
        QString birthday; // YYYY-MM-DD or --MM-DD, or empty
        QList<ContactPhone> phones;
    };

    // Every card in the file, including cards without an email address;
    // the importer decides what to do with those.
    static QList<Card> parse(const QByteArray &data);
    static QByteArray emit(const QList<ContactInfo> &contacts);

    // A birthday in any form vCards use, as YYYY-MM-DD or --MM-DD; empty if
    // it is not a date.
    static QString normalizeBirthday(const QString &value);
};

} // namespace e3::mail
