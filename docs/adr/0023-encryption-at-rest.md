# 0023 — Encryption at rest: the database and blobs, under a per-account key

**Status:** Accepted — 2026-10-21

## Context
Until now everything e3mail kept on disk was in the clear:
- message bodies, contacts and the op-log in SQLite;
- attachments and raw originals as files named by their SHA-256;
- secret keys in the database;
- passwords too, where there is no OS keyring (ADR 0012).

A stolen laptop or a copied home directory gave all of it away. Phase 11 asks
for an encrypted database and encrypted blobs. Encrypted backups (ADR 0020)
were the third part of the phase and are done.

The person chose SQLite3 Multiple Ciphers over SQLCipher packages. SQLCipher
would have meant OpenSSL in the Windows and Android builds and four SQLCipher
versions across platforms.

## Decision
- **SQLite3 Multiple Ciphers 2.5.1** (SQLite 3.53.4, MIT).
  - It is fetched by CMake at a pinned release and hash, and built
    statically with FTS5 on every platform (`cmake/E3mailSqlite.cmake`).
  - The system SQLite, vcpkg's port and the CI packages are gone.
- **Every page of the database is encrypted** with ChaCha20-Poly1305, under
  a random 256-bit key per account.
  - Key stretching is pointless for a random key, so `kdf_iter` is 1.
  - A wrong key fails when the database is opened, not later.
- **Blobs are sealed** with XChaCha20-Poly1305, reusing ADR 0020's
  primitives.
  - The key is HMAC(database key, "e3mail blobs v1"); each blob is bound to
    its name.
  - A file is named by HMAC(blob key, SHA-256), so the disk shows neither
    the content nor which known file is present.
  - The database still names blobs by SHA-256, as devices do between
    themselves.
  - What is worth copying or backing up is listed from the database
    (`BlobStore::referenced`), not from the directory.
- **The key is guarded** in `<account>/key.json`, by:
  - **the OS keyring** (`account-<id>-dbkey`), by default, which opens
    unattended;
  - **an app passphrase**, when the person sets one in Settings. The key is
    sealed with Argon2id (64 MiB, 3 passes) and XChaCha20-Poly1305. The app
    asks for the passphrase before opening anything; the CLI reads
    `E3MAIL_PASSPHRASE`. One passphrase guards every account.
  - **nothing**, where there is neither: the key sits beside the mail, and
    Settings says so. That is no protection against someone with the disk;
    it keeps the file format the same.
  - Whenever an account is opened, its key is re-guarded as the machine now
    allows (a keyring appeared, a passphrase was set or removed).
- **Upgrading is automatic.**
  - On first open, a database an older version wrote in the clear is
    encrypted in place (`PRAGMA rekey`, after a WAL checkpoint).
  - Its blobs are sealed one by one, each before its clear copy is removed,
    resuming after a crash. key.json records when they are done.
- **Copies carry keys of their own.**
  - A snapshot for a backup or a new device is encrypted with a fresh random
    key, never the account's.
  - That key travels in the backup's sealed manifest (`secrets.dbkey`), or
    in the pairing manifest inside the authenticated channel.
  - A restore or a join stages the copy under a new key and rekeys the
    database to it. Backups and offers from before this change carry no key
    and are plaintext, which rekeying handles the same way.

## Consequences
- A forgotten passphrase loses the mail on this device. A backup or another
  device restores it; the dialog says so.
- Anyone who can sign in as the person can read the keyring, and so the
  mail, without a passphrase. That is the same trust ADR 0012 gives
  passwords.
- Passwords that fell back to the database (ADR 0012) are now inside an
  encrypted file, but guarded only as well as its key.
- Memory, swap and the OS's own caches are out of scope. So are temporary
  files the person saves attachments to.
- `e3mail-cli` without `E3MAIL_PASSPHRASE` refuses to open a guarded data
  directory rather than prompting.
- Opening a database costs one key check. Opening a passphrase-guarded
  account costs one Argon2id per process, cached per salt.
