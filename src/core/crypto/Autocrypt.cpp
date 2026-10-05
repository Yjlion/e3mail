// SPDX-License-Identifier: MPL-2.0
#include "Autocrypt.h"

namespace e3::crypto {

std::optional<AutocryptHeader> Autocrypt::parse(const QByteArray &value)
{
    AutocryptHeader h;
    bool haveKey = false;
    for (const QByteArray &rawAttr : value.split(';')) {
        const QByteArray attr = rawAttr.trimmed();
        if (attr.isEmpty())
            continue;
        const qsizetype eq = attr.indexOf('=');
        if (eq < 0)
            return std::nullopt;
        const QByteArray name = attr.left(eq).trimmed().toLower();
        const QByteArray val = attr.mid(eq + 1).trimmed();
        if (name == "addr") {
            h.addr = QString::fromUtf8(val).trimmed().toLower();
        } else if (name == "prefer-encrypt") {
            h.preferEncrypt = val.trimmed().toLower() == "mutual";
        } else if (name == "keydata") {
            QByteArray compact = val;
            compact.replace(' ', "").replace('\t', "").replace('\r', "").replace('\n', "");
            h.keydata = QByteArray::fromBase64(compact);
            haveKey = !h.keydata.isEmpty();
        } else if (!name.startsWith('_')) {
            return std::nullopt; // unknown critical attribute
        }
    }
    if (h.addr.isEmpty() || !haveKey)
        return std::nullopt;
    return h;
}

std::optional<AutocryptHeader> Autocrypt::select(const QList<QByteArray> &values, const QString &fromAddr)
{
    std::optional<AutocryptHeader> found;
    const QString from = fromAddr.trimmed().toLower();
    for (const QByteArray &v : values) {
        auto h = parse(v);
        if (!h || h->addr != from)
            continue;
        if (found)
            return std::nullopt;
        found = h;
    }
    return found;
}

QByteArray Autocrypt::format(const QString &addr, const QByteArray &keydata, bool preferEncrypt)
{
    QByteArray out = "addr=" + addr.toUtf8() + ';';
    if (preferEncrypt)
        out += " prefer-encrypt=mutual;";
    out += " keydata=";
    const QByteArray b64 = keydata.toBase64();
    for (qsizetype i = 0; i < b64.size(); i += 72) {
        if (i)
            out += ' ';
        out += b64.mid(i, 72);
    }
    return out;
}

} // namespace e3::crypto
