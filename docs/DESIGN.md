# e3mail — Design

## What and why

eeemail showed that Delta Chat's ideas — Autocrypt, QR verification, a local
mailbox fed by a server used only as a spool — make a good *classic* email
client. It got there by forking Delta Chat's Rust engine, and spent much of its
effort working around the engine being a chat engine: messages belonged to
chats, not recipient sets, there was no Cc, and no way to receive anything but
IMAP.

e3mail keeps the ideas and writes the engine for email from the start, in C++
on Qt, so that one toolkit carries it to Linux, Windows and macOS the way VLC
does. Owning the transport also means POP3 support, which a download-and-delete
mailbox suits naturally.

## Architecture

```
src/core/      libe3core: the whole engine. QtCore + QtNetwork, no GUI.
  util/        data directory and portable mode, hybrid logical clock, logging
  store/       SQLite wrapper, migrations, config, content-addressed blobs, op-log
  mime/        parsing and building (RFC 2045-2049, 2047, 2231, 5322), HTML sanitizer
  net/         SMTP, IMAP, POP3 over one event-loop-backed socket class
  crypto/      RNP wrapper, Autocrypt headers
  mail/        ingest, compose, policy, threading, tags, search, contacts, housekeeping
  engine/      Account, AccountManager, MailWorker, SecretStore, Autoconfig
src/app/       the QML desktop app (controllers, models, qml/)
src/cli/       e3mail-cli, a headless driver for the same engine
tests/         Qt Test suites, the fake mail system, the fixture generator
server/compose Postfix + Dovecot test server (IMAP and POP3, TLS)
packaging/     desktop file, icons, AppImage script
```

The engine is a library with no GUI dependency. The app, the CLI and the tests
call the same code in-process, so nothing the app does goes untested for want
of a separate IPC path. eeemail's desktop app ran its engine over IPC that no
test reached, and twice shipped with that path broken.

### Threads

Each account has a **worker thread** with its own database connection and its
own RNP keyring. It sends the outbox, fetches (IMAP IDLE, or POP3 polling),
applies server retention and runs housekeeping. The UI thread has a second
connection and keyring for reading and composing. They meet in SQLite (WAL
mode) and in queued Qt signals. Keys live in the database and are loaded into
each keyring on demand.

### The data model

Messages are email-native: each has its own recipient set (To, Cc and Bcc,
with Bcc never written to a header), its own subject, and a thread derived from
`References` and `In-Reply-To`, never from the subject. See
`src/core/store/Schema.cpp`; every table is commented.

System tags are not all stored. Inbox, Sent and Drafts are *derived* from
direction, state and the absence of other tags. Archive is a reserved label,
and Trash and Unverified have tables of their own because each carries a
deadline. Every list view is one `SearchQuery`.

### Receive

`mail::Ingest::process` takes one raw message to one stored message in one
transaction:

1. Parse. If the message is PGP/MIME, decrypt it and let RFC 9788 protected
   headers win over the outer ones.
2. Deduplicate by Message-ID.
3. Record contacts, learn the sender's Autocrypt key (unverified) and gossiped
   keys (never overriding a key the person sent themselves), and check the
   signature, re-verifying if this message is what taught us the key.
4. Store bodies, recipients, attachments and the retained original. Thread and
   index it.
5. Decide where it lands. A blocked sender goes to Trash, cleartext under
   strict mode goes to Trash, and a stranger goes to Unverified.

### Send

`mail::Compose::queue` evaluates the policy, renders the message (encrypting to
every recipient and to ourselves, with an Autocrypt header outside and gossip
inside), stores it as Pending and puts it in the outbox. Writing to someone
makes them known, which releases any of their mail held in Unverified.

**Opportunistic encryption means all or nothing.** eeemail inherited from
Delta Chat a behaviour where an encrypted message silently dropped recipients
without keys, and could only report it afterwards. e3mail encrypts only when
every recipient has a key, sends cleartext to all otherwise, and says which
before the user presses Send. This is what Autocrypt recommends.
See [ADR 0004](adr/0004-all-or-nothing-encryption.md).

### Untrusted content

There are two independent barriers between a message and the network:

1. `mime::sanitizeHtml` re-emits a fixed set of tags and attributes. It drops
   scripts, styles and every remote reference, and counts what it dropped so
   the reading pane can say so.
2. The QML engine has a network manager (`src/app/NetworkBarrier.cpp`) that
   refuses every request that is not for the app's own resources.

HTML renders in a read-only `TextEdit`, not a web engine, so nothing in a
message can run. Links open through a dialog that shows the real target.

## Multi-client: devices sync with each other, not with the server

The server is a spool and each device's database is the mailbox, so a second
device sees nothing the first has already downloaded and deleted. Tags, read
state, trash, contacts, keys and verifications don't travel either.

**Iroh for device-to-device transport**, plus rules that keep things correct
when devices are never online together:

1. **One small Rust crate, `src/p2p/` (`e3p2p`)**, wraps `iroh` and
   `iroh-blobs` behind a narrow non-blocking C ABI (cbindgen), built from
   CMake with Corrosion. Rust only moves bytes and blobs; protocol logic stays
   in C++ behind a `DeviceTransport` interface. Iroh's official C binding was
   considered: it is itself a Rust crate built with cargo, so it does not
   remove the Rust toolchain, and it lacks `iroh-blobs`.
2. **A replicated op-log** (`ops` table, in the schema since migration 1):
   every user-visible change is recorded with a hybrid logical clock. Ops name
   messages by Message-ID, never by local row id. Merge is last-writer-wins
   per field, tag sets are add/remove sets, and a purge is a tombstone.
3. **Pairing by QR**: the new device gets a full encrypted snapshot (database
   plus blobs over `iroh-blobs`). Account secrets travel only inside this
   authenticated channel.
4. **Every device fetches, and server deletion waits for every paired device's
   acknowledgement.** "Delete after download" becomes "delete after every
   device has it". This works the same for IMAP UIDs and POP3 UIDLs, so devices
   that are never online together still converge through the server.
5. **A mailbox fallback**: when no peer is reachable, ops are also mailed to
   self, encrypted and hidden, and kept on the server by rule 4.

The op-log and the device id exist now. The worker's deletion rule is written
in terms of "stored here", which is trivially "every device" with one device.

## Phases

| Phase | | State |
|---|---|---|
| P0 | Foundation: CMake, presets, CI, data directory, portable mode | ✅ |
| P1 | Store: SQLite, migrations, blobs, raw-message retention, op-log | ✅ |
| P2 | MIME: parse, build, encodings, sanitizer | ✅ |
| P3 | Transport and accounts: SMTP, IMAP, POP3, autoconfig, worker, retention | ✅ |
| P4 | Crypto: RNP, Autocrypt, PGP/MIME, protected headers, policy, padlock | ✅ |
| P5 | Organisation: threads, tags, search, Unverified, Trash, blocklist, importance | ✅ |
| P6 | Desktop app (QML) | ✅ |
| P7 | Packaging and CI: `.deb`, AppImage, NSIS, portable zip, `.dmg` | ✅ CI green on all three; release workflow not yet run |
| P8 | Multi-client over Iroh | designed |
| P9 | SecureJoin and QR (Delta Chat compatible) | |
| P10 | Address book: details, phones, categories | |
| P11 | At rest: SQLCipher passphrase, blob encryption, encrypted backup | |
| P12 | Read receipts (MDN) and disappearing messages into Trash | |
| P13 | Structured email (SML), trusted vs inert | |

## Verification

- `ctest --preset dev`:
  - unit tests for MIME, the sanitizer and the composer's whitelist;
  - protocol tests against the fake mail system;
  - crypto round trips, including GnuPG decrypting and verifying our output;
  - an engine test where real accounts exchange mail end to end;
  - a smoke test that boots the real QML app offscreen.
- `scripts/e2e.py` against `server/compose`: real TLS (implicit and STARTTLS)
  on every protocol, both receive paths, and retention observed from the
  server's side.
- `scripts/screenshots.sh`: renders every view from fixtures.
