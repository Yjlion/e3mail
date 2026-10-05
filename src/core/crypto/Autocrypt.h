// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace e3::crypto {

// Autocrypt Level 1 headers. An incoming header is unauthenticated: anyone who
// can write the From line can write it. It earns a key, never verification.
struct AutocryptHeader
{
    QString addr;
    bool preferEncrypt = false;  // prefer-encrypt=mutual
    QByteArray keydata;          // binary OpenPGP key
};

class Autocrypt
{
public:
    // Parses one header value. Returns nullopt when it must be ignored: no
    // addr or keydata, or an unknown attribute without a leading underscore.
    static std::optional<AutocryptHeader> parse(const QByteArray &value);

    // Picks the header for `fromAddr` out of all Autocrypt headers on a
    // message; more than one valid header for the address means none.
    static std::optional<AutocryptHeader> select(const QList<QByteArray> &values, const QString &fromAddr);

    // Formats for the Autocrypt (or Autocrypt-Gossip) header, base64 keydata
    // broken by spaces so the header folds within line limits.
    static QByteArray format(const QString &addr, const QByteArray &keydata, bool preferEncrypt);
};

} // namespace e3::crypto
