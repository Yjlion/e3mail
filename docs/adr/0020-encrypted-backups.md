# 0020 — Encrypted backups, to a place the person chooses

**Status:** Accepted — 2026-10-17

## Context
The database is the mailbox (ADR 0006): once mail is deleted from the
server, the only copies are on the person's devices. Android's own backup is
off because it would copy the secret key unencrypted (ADR 0016). Pairing
(ADR 0010) copies an account to another device, but both can be lost
together.

## Decision
- **Encrypted on the device, before anything leaves it.** A random 256-bit
  master key encrypts everything. It is stored in the repository wrapped by
  a key from the person's passphrase, using Argon2id (64 MiB, 3 passes, with
  the parameters stored and bounded when read). Instead of a passphrase, a
  generated recovery code of 25 characters can be used. Without one or the
  other, nobody can open the backups. On the device, the master key is kept
  in the OS keyring (ADR 0012's fallback applies), so daily backups need no
  passphrase.
- **A repository is a store of named objects:**
  - `key`;
  - `objects/<hex>`: each blob, and the database in pieces of 8 MiB, each
    compressed and sealed with XChaCha20-Poly1305 bound to its name;
  - `snapshots/<device>-<time>`: sealed manifests.

  Object names are an HMAC of the content's SHA-256, so the place cannot test
  whether it holds a known message. It sees ciphertext, sizes, times, and how
  many devices back up.
- **Incremental by content.** Blobs are already named by their SHA-256. A
  backup lists what is there and sends only what is not. The database copy
  is `VACUUM INTO`, without the device's own settings (`cfg::DeviceOnly`,
  shared with pairing).
- **Every backup is read back.** After writing, e3mail checks:
  - that the manifest decrypts;
  - that every object it names is there;
  - that every database piece and 16 random blobs decrypt and match their
    hashes.

  Only then is it recorded as done.
- **Retention:** 7 daily, 4 weekly and 6 monthly snapshots per device; the
  newest always stays. Objects no snapshot needs are removed after a day, in
  case a backup on another device is still writing.
- **One repository per account; devices back up on their own.** Each device
  is set up with the same passphrase. Setting up where a repository exists
  opens it, and a wrong passphrase is refused. Nothing about backups travels
  with pairing.
- **Restoring makes a new device of the account**, with no peers: it can be
  paired again. The passwords and the OAuth refresh token are in the sealed
  manifest, so it connects at once.
- **Places:**
  - a folder;
  - WebDAV;
  - S3 and compatibles (Signature Version 4, a bucket made if missing);
  - SFTP (libssh; the host key shown and pinned, as ADR 0017 does for
    certificates);
  - Google Drive (`drive.file`, a folder the person can see);
  - OneDrive (the app's folder).

  Drive and OneDrive sign in with ADR 0019's clients and their own refresh
  token. Our own clients, not rclone: one program to ship and to trust.
- **Daily, while e3mail runs** (on Android, also under ADR 0018's
  service), and on demand. `e3mail-cli` has `backup-setup`, `backup-now`,
  `backup-list`, `backup-forget` and `restore`.

## Consequences
- Message content now leaves the device to a place other than the mail
  server, encrypted (ADR 0009 amendment).
- SFTP is not in the Android build yet: vcpkg's libssh brings an OpenSSL of
  its own beside the one Qt loads there.
- Sizes and times are visible to the place; nothing pads them.
- A passphrase that is lost cannot be recovered by anyone.
