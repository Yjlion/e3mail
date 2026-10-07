// SPDX-License-Identifier: MPL-2.0
#include "Schema.h"

#include "Database.h"
#include "util/Log.h"

#include <array>

namespace e3 {

namespace {

// Never edit a migration that has shipped; add a new one.
constexpr std::array kMigrations = {
    // 1 — the mailbox. Email-native from the start: a message has its own
    // recipient set and belongs to a thread, never to a "chat".
    R"sql(
CREATE TABLE config (
    key   TEXT PRIMARY KEY,
    value TEXT
);

CREATE TABLE messages (
    id              INTEGER PRIMARY KEY,
    message_id      TEXT NOT NULL UNIQUE,      -- RFC 5322 Message-ID, without <>
    direction       INTEGER NOT NULL,          -- 0 incoming, 1 outgoing
    state           INTEGER NOT NULL,          -- see MessageState
    subject         TEXT NOT NULL DEFAULT '',
    from_addr       TEXT NOT NULL DEFAULT '',
    from_name       TEXT NOT NULL DEFAULT '',
    reply_to        TEXT,
    date            INTEGER NOT NULL,          -- ms since epoch, from Date:
    received_at     INTEGER NOT NULL,
    in_reply_to     TEXT,
    body_text       TEXT NOT NULL DEFAULT '',
    body_html       TEXT,                      -- as received; sanitized when rendered
    preview         TEXT NOT NULL DEFAULT '',
    importance      INTEGER NOT NULL DEFAULT 0,-- -1 low, 0 normal, 1 high
    encrypted       INTEGER NOT NULL DEFAULT 0,
    signed_fpr      TEXT,                      -- fingerprint of a valid signature
    verified        INTEGER NOT NULL DEFAULT 0,-- signer was a verified contact at receive
    has_attachments INTEGER NOT NULL DEFAULT 0,
    raw_blob        TEXT,                      -- retained original, see RawMime
    raw_expires_at  INTEGER,                   -- NULL keeps it forever
    thread_id       INTEGER,
    hidden          INTEGER NOT NULL DEFAULT 0,-- device-sync and other machine mail
    send_encryption INTEGER,                   -- composer padlock; NULL is auto
    unread          INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX messages_date ON messages(date);
CREATE INDEX messages_thread ON messages(thread_id);

CREATE TABLE recipients (
    msg_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    kind   INTEGER NOT NULL,                   -- 0 to, 1 cc, 2 bcc
    idx    INTEGER NOT NULL,
    addr   TEXT NOT NULL,
    name   TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (msg_id, kind, idx)
);
CREATE INDEX recipients_addr ON recipients(addr);

CREATE TABLE msg_refs (
    msg_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    idx    INTEGER NOT NULL,
    ref    TEXT NOT NULL,
    PRIMARY KEY (msg_id, idx)
);

CREATE TABLE attachments (
    id         INTEGER PRIMARY KEY,
    msg_id     INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    idx        INTEGER NOT NULL,
    filename   TEXT NOT NULL,
    mime_type  TEXT NOT NULL,
    size       INTEGER NOT NULL,
    blob       TEXT NOT NULL,
    content_id TEXT
);
CREATE INDEX attachments_msg ON attachments(msg_id);

-- Threads group by reference chain only, never by subject.
CREATE TABLE threads (
    id            INTEGER PRIMARY KEY,
    last_activity INTEGER NOT NULL
);
CREATE TABLE thread_refs (
    message_id TEXT PRIMARY KEY,
    thread_id  INTEGER NOT NULL REFERENCES threads(id) ON DELETE CASCADE
);

-- Tags. System rows (Archive, Trash, Unverified) are stored because each is a
-- user decision or carries a deadline; Inbox, Sent and Drafts are derived.
CREATE TABLE labels (
    id        INTEGER PRIMARY KEY,
    name      TEXT NOT NULL,
    name_norm TEXT NOT NULL UNIQUE,
    color     TEXT NOT NULL DEFAULT '',
    system    INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE msg_labels (
    msg_id   INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    label_id INTEGER NOT NULL REFERENCES labels(id) ON DELETE CASCADE,
    PRIMARY KEY (msg_id, label_id)
);

-- Trash is the only place that destroys mail on a timer.
CREATE TABLE trashed (
    msg_id     INTEGER PRIMARY KEY REFERENCES messages(id) ON DELETE CASCADE,
    trashed_at INTEGER NOT NULL,
    purge_at   INTEGER NOT NULL,
    reason     INTEGER NOT NULL                -- see TrashReason
);
-- Mail from strangers waits here. The deadline is held_at plus the setting
-- read at sweep time, deliberately not stored.
CREATE TABLE held (
    msg_id  INTEGER PRIMARY KEY REFERENCES messages(id) ON DELETE CASCADE,
    held_at INTEGER NOT NULL
);

CREATE TABLE contacts (
    id             INTEGER PRIMARY KEY,
    addr           TEXT NOT NULL UNIQUE,       -- normalised: lower case
    name           TEXT NOT NULL DEFAULT '',
    origin         INTEGER NOT NULL DEFAULT 0, -- see ContactOrigin
    fingerprint    TEXT,                       -- current key, if any
    verified       INTEGER NOT NULL DEFAULT 0, -- SecureJoin completed
    prefer_encrypt INTEGER NOT NULL DEFAULT 0, -- Autocrypt prefer-encrypt=mutual
    key_seen_at    INTEGER,                    -- Date of the message the key came from
    last_seen      INTEGER
);

CREATE TABLE keys (
    fingerprint TEXT PRIMARY KEY,
    data        BLOB NOT NULL,                 -- binary transferable key
    secret      INTEGER NOT NULL DEFAULT 0,
    created_at  INTEGER NOT NULL
);

CREATE TABLE contact_policy (
    contact_id      INTEGER PRIMARY KEY REFERENCES contacts(id) ON DELETE CASCADE,
    encryption_mode INTEGER                    -- NULL inherits the account mode
);

CREATE TABLE blocklist (
    pattern_norm TEXT PRIMARY KEY,
    pattern      TEXT NOT NULL,
    created_at   INTEGER NOT NULL
);

-- Recipients who were addressed but did not get the message, for want of a key.
CREATE TABLE undelivered (
    msg_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    addr   TEXT NOT NULL,
    PRIMARY KEY (msg_id, addr)
);

-- What the server holds. remote_key is "imap:<uidvalidity>:<uid>" or
-- "pop3:<uidl>". Mail that was already on the server the first time this
-- account looked is preexisting and is never deleted: retention is not
-- retroactive.
CREATE TABLE server_msgs (
    remote_key  TEXT PRIMARY KEY,
    first_seen  INTEGER NOT NULL,
    msg_id      INTEGER,
    preexisting INTEGER NOT NULL DEFAULT 0,
    deleted     INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE outbox (
    id           INTEGER PRIMARY KEY,
    msg_id       INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    raw          BLOB NOT NULL,
    mail_from    TEXT NOT NULL,
    rcpts        TEXT NOT NULL,                -- newline separated
    attempts     INTEGER NOT NULL DEFAULT 0,
    next_attempt INTEGER NOT NULL DEFAULT 0,
    last_error   TEXT
);

-- Replicated operation log: every user-visible change, stamped with a hybrid
-- logical clock, so another device can replay it (multi-client sync).
CREATE TABLE ops (
    id      INTEGER PRIMARY KEY,
    device  TEXT NOT NULL,
    seq     INTEGER NOT NULL,
    hlc     INTEGER NOT NULL,
    kind    TEXT NOT NULL,
    payload TEXT NOT NULL,
    UNIQUE (device, seq)
);

-- Archive is a reserved label; Trash and Unverified have tables of their own
-- because they carry deadlines.
INSERT INTO labels(name, name_norm, system) VALUES ('Archive', 'archive', 1);

CREATE VIRTUAL TABLE msg_fts USING fts5(
    subject, body, people,
    tokenize = 'unicode61 remove_diacritics 2'
);
)sql",

    // 2 — merging ops from other devices (ADR 0013).
    R"sql(
-- What an op competes with: ops with the same key are last-writer-wins by
-- (hlc, device). NULL for ops that only ever add. See OpLog::mergeKey.
ALTER TABLE ops ADD COLUMN mkey TEXT;
UPDATE ops SET mkey = CASE kind
    WHEN 'msg.read'     THEN 'read:' || json_extract(payload, '$.mid')
    WHEN 'msg.trash'    THEN 'trash:' || json_extract(payload, '$.mid')
    WHEN 'msg.restore'  THEN 'trash:' || json_extract(payload, '$.mid')
    WHEN 'label.add'    THEN 'label:' || json_extract(payload, '$.mid') || ':' || lower(json_extract(payload, '$.label'))
    WHEN 'label.remove' THEN 'label:' || json_extract(payload, '$.mid') || ':' || lower(json_extract(payload, '$.label'))
    WHEN 'label.define' THEN 'labeldef:' || lower(json_extract(payload, '$.name'))
    WHEN 'label.delete' THEN 'labeldef:' || lower(json_extract(payload, '$.name'))
    WHEN 'block.add'    THEN 'block:' || json_extract(payload, '$.pattern')
    WHEN 'block.remove' THEN 'block:' || json_extract(payload, '$.pattern')
    WHEN 'contact.edit' THEN 'cname:' || json_extract(payload, '$.addr')
END;
CREATE INDEX ops_mkey ON ops(mkey, hlc);
CREATE INDEX ops_hlc ON ops(hlc);

-- Message-IDs purged here or on another device. A tombstone outranks every
-- later op and every re-download.
CREATE TABLE tombstones (
    message_id TEXT PRIMARY KEY,
    purged_at  INTEGER NOT NULL
);

-- The other devices paired with this one. Empty is the single-device case.
CREATE TABLE devices (
    id        TEXT PRIMARY KEY,
    name      TEXT NOT NULL DEFAULT '',
    paired_at INTEGER NOT NULL,
    last_seen INTEGER
);

-- Which device has stored which server message. The server copy is deleted
-- only once every paired device is here (ADR 0013).
CREATE TABLE server_acks (
    remote_key TEXT NOT NULL,
    device     TEXT NOT NULL,
    PRIMARY KEY (remote_key, device)
);
)sql",

    // 3 — the address book (P10).
    R"sql(
ALTER TABLE contacts ADD COLUMN organization TEXT NOT NULL DEFAULT '';
ALTER TABLE contacts ADD COLUMN title        TEXT NOT NULL DEFAULT '';
ALTER TABLE contacts ADD COLUMN notes        TEXT NOT NULL DEFAULT '';
ALTER TABLE contacts ADD COLUMN birthday     TEXT NOT NULL DEFAULT ''; -- YYYY-MM-DD or --MM-DD

CREATE TABLE contact_phones (
    contact_id INTEGER NOT NULL REFERENCES contacts(id) ON DELETE CASCADE,
    idx        INTEGER NOT NULL,
    label      TEXT NOT NULL DEFAULT '',   -- mobile | work | home | other
    number     TEXT NOT NULL,
    PRIMARY KEY (contact_id, idx)
);
)sql",
};

} // namespace

int Schema::currentVersion()
{
    return int(kMigrations.size());
}

void Schema::migrate(Database &db)
{
    int version = db.userVersion();
    while (version < currentVersion()) {
        Transaction tx(db);
        db.exec(kMigrations[size_t(version)]);
        ++version;
        db.setUserVersion(version);
        tx.commit();
        qCInfo(lcStore) << "migrated" << db.path() << "to schema" << version;
    }
}

} // namespace e3
