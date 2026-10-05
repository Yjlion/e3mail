// SPDX-License-Identifier: MPL-2.0
#include "Config.h"

#include "Database.h"

#include <cstring>

namespace e3 {

QString Config::defaultFor(const char *key)
{
    struct Default
    {
        const char *key;
        const char *value;
    };
    // UnverifiedTrashDays and TrashPurgeDays read 0 in opposite ways on
    // purpose: 0 days in Trash means "destroy at once", while 0 days in
    // Unverified means "never sweep" (someone who wants strangers' mail gone at
    // once turns gating off instead).
    static constexpr Default kDefaults[] = {
        {cfg::Protocol, "imap"},
        {cfg::InSecurity, "ssl"},
        {cfg::SmtpSecurity, "ssl"},
        {cfg::EncryptionMode, "1"},
        {cfg::ServerRetention, "delete"},
        {cfg::ServerKeepDays, "30"},
        {cfg::RawMimeDays, "30"},
        {cfg::TrashPurgeDays, "30"},
        {cfg::UnverifiedTrashDays, "30"},
        {cfg::Gating, "1"},
        {cfg::PollSeconds, "300"},
    };
    for (const auto &d : kDefaults) {
        if (std::strcmp(d.key, key) == 0)
            return QString::fromLatin1(d.value);
    }
    return QString();
}

QString Config::get(const char *key) const
{
    if (auto v = m_db.queryText("SELECT value FROM config WHERE key=?", key))
        return *v;
    return defaultFor(key);
}

int Config::getInt(const char *key) const
{
    return get(key).toInt();
}

void Config::set(const char *key, const QString &value)
{
    m_db.run("INSERT INTO config(key, value) VALUES(?, ?) "
             "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
             key, value);
}

bool Config::isSet(const char *key) const
{
    return m_db.queryInt("SELECT 1 FROM config WHERE key=?", key).has_value();
}

} // namespace e3
