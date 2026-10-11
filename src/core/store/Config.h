// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>

namespace e3 {

class Database;

// Per-account settings in the `config` table. Every key and its default is
// declared here and nowhere else, so a reader can see the whole surface.
namespace cfg {
// Identity
inline constexpr char Addr[] = "addr";
inline constexpr char DisplayName[] = "display_name";
inline constexpr char Signature[] = "signature";             // before signatures were named; see Signatures
inline constexpr char SignaturePrefix[] = "signature:";      // + id: {"name", "html", "text"}, empty when removed
inline constexpr char SignatureDefault[] = "signature_default"; // an id, or empty for none
// Incoming server
inline constexpr char Protocol[] = "protocol";               // "imap" | "pop3"
inline constexpr char InHost[] = "in_host";
inline constexpr char InPort[] = "in_port";
inline constexpr char InSecurity[] = "in_security";          // "ssl" | "starttls" | "plain"
inline constexpr char InUser[] = "in_user";
inline constexpr char InCertSha256[] = "in_cert_sha256";     // a pinned certificate (ADR 0017)
// Outgoing server
inline constexpr char SmtpHost[] = "smtp_host";
inline constexpr char SmtpPort[] = "smtp_port";
inline constexpr char SmtpSecurity[] = "smtp_security";
inline constexpr char SmtpUser[] = "smtp_user";
inline constexpr char SmtpCertSha256[] = "smtp_cert_sha256";
// Every certificate accepted, from e3mail-cli --insecure only; never set by the app.
inline constexpr char AcceptInvalidCerts[] = "accept_invalid_certs";
// Fallback secret storage when no OS keyring is available. See SecretStore.
inline constexpr char InPassword[] = "in_password";
inline constexpr char SmtpPassword[] = "smtp_password";
// Policy
inline constexpr char EncryptionMode[] = "encryption_mode";  // 0 lenient, 1 opportunistic, 2 strict
inline constexpr char ServerRetention[] = "server_retention";// "delete" | "keep" | "never"
inline constexpr char ServerKeepDays[] = "server_keep_days";
inline constexpr char RawMimeDays[] = "raw_mime_days";        // 0 off, N days, -1 forever
inline constexpr char TrashPurgeDays[] = "trash_purge_days";  // 0 destroys at once
inline constexpr char UnverifiedTrashDays[] = "unverified_trash_days"; // 0 never sweeps
inline constexpr char Gating[] = "gating";                    // hold strangers' mail
inline constexpr char PollSeconds[] = "poll_seconds";
// Engine state
inline constexpr char DeviceId[] = "device_id";
inline constexpr char SelfFingerprint[] = "self_fingerprint";
inline constexpr char ImapUidValidity[] = "imap_uidvalidity";
inline constexpr char FirstSyncDone[] = "first_sync_done";
// IMAP cursors, per UIDVALIDITY: UIDs below the baseline were on the server
// before e3mail first looked; high is the newest UID handled, and backfill
// (newest first) has handled everything from low up.
inline constexpr char ImapBaseline[] = "imap_baseline";
inline constexpr char ImapHigh[] = "imap_high";
inline constexpr char ImapLow[] = "imap_low";
inline constexpr char Configured[] = "configured";
// Devices (ADR 0010). Per device: never synced, never copied when pairing.
inline constexpr char P2pSecret[] = "p2p_secret";            // this device's Iroh key, hex
inline constexpr char P2pRelay[] = "p2p_relay";              // "default" | "off" | a relay URL
inline constexpr char DeviceName[] = "device_name";          // what other devices call this one
} // namespace cfg

class Config
{
public:
    explicit Config(Database &db) : m_db(db) {}

    QString get(const char *key) const;
    int getInt(const char *key) const;
    bool getBool(const char *key) const { return getInt(key) != 0; }
    void set(const char *key, const QString &value);
    void setInt(const char *key, int value) { set(key, QString::number(value)); }
    bool isSet(const char *key) const;

    static QString defaultFor(const char *key);

private:
    Database &m_db;
};

} // namespace e3
