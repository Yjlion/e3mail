# To do

Planned work that has not started. Each item says what exists to build on
and what is still open. When one starts, it gets an ADR (or an amendment) and
a section in [`docs/handoff.md`](docs/handoff.md); remove it from here when it
is done.

## Multi-client sync over Iroh

Phase 8, slices 2 and 3 of [DESIGN.md](docs/DESIGN.md);
[ADR 0010](docs/adr/0010-multi-client-over-iroh.md) and
[ADR 0013](docs/adr/0013-op-merge-rules.md).

**There already:**
- The op-log, its merge rules, tombstones and deferred ops (`src/core/sync/`).
- `SyncSession` (anti-entropy) and the `DeviceTransport` interface Iroh sits
  behind.
- `tst_sync`, with two devices over a loopback transport.

**To build:**
- A Rust crate in `src/p2p/` (Iroh and `iroh-blobs`), built with Corrosion.
- The Rust toolchain in CI on all three desktop platforms, and on Android.
- A `SyncSession` wired into `Account`.
- Pairing by QR code with an encrypted snapshot.
- The mailbox fallback, for devices that are never online together.

**Open:**
- How Android keeps a peer reachable with no background service
  (ADR 0016).
- Whether the relay is Iroh's public one or one the person runs.

## Encrypted backups to cloud storage

Destinations: Google Drive, OneDrive, S3 and compatible, SFTP, WebDAV
(Nextcloud and others). Part of P11 ("at rest").

**There already:** the account directory is a SQLite database plus a blob
store. Android's own backup is turned off on purpose (ADR 0016), because it
would copy the secret key unencrypted.

**Shape:**
- Encrypted on the device before anything leaves it. The key comes from a
  passphrase (Argon2id), or the account's OpenPGP key with a printed
  recovery code.
- The provider only ever sees ciphertext and sizes.
- Incremental: blobs are content-addressed already, so only new ones go up.
- A restore is tested on every backup (decrypt the manifest, check hashes).

**Open:**
- OAuth for Google and Microsoft. There is no OAuth in e3mail yet, and their
  app registration and review are a project of their own.
- Whether to use rclone as a helper or our own clients. rclone covers every
  destination but is a second program to ship and trust.
- How backups interact with P8: are they per device or per account?
- Retention and pruning of old backups.

## Optional local AI through llama.cpp

Summaries, reply drafts, smarter search and triage, all on the device.

**Shape:**
- Off by default. The setting that turns it on says, before downloading
  anything, what it needs:
  - the model's size on disk;
  - the memory it uses while running;
  - whether a GPU is used;
  - roughly how long an answer takes on this machine.
- llama.cpp linked in, or run as a local `llama-server` on localhost only.
- Models in GGUF, downloaded once from a source the person chooses, or a
  file they pick.
- Message content goes to the local model and nowhere else. It must stay
  inside ADR 0009's barriers: a new ADR says how the model process is kept
  off the network.

**Open:**
- Which tasks are worth it on modest hardware.
- Android, where the memory budget is small.
- How a summary is marked as machine-made in the interface.

## Server integration with Mox and Maddy

For people who run their own mail server: change the password, manage
filters (Sieve), aliases, quotas and spam settings from e3mail.

**There already:** `server/compose` (Postfix and Dovecot) for testing.

**Shape:**
- Mox has an admin and account web API (JSON over HTTP). Maddy has a CLI
  (`maddy creds`, `maddy imap-acct`) and no HTTP API, so it needs another
  route: ManageSieve (RFC 5804) for filters, and SSH or a small agent for the
  rest.
- Detected from the server (autoconfig, the Mox web interface), never
  assumed.
- Credentials as for mail (ADR 0012).

**Open:**
- ManageSieve alone would give filters on any Sieve server (Dovecot too);
  perhaps that comes first.
- Test servers for Mox and Maddy beside `server/compose`.
