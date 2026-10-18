# To do

Planned work that has not started. Each item says what exists to build on
and what is still open. When one starts, it gets an ADR (or an amendment) and
a section in [`docs/handoff.md`](docs/handoff.md); remove it from here when it
is done.

## Multi-client sync: what is left

Phase 8 is built (see `docs/handoff.md`, ADR 0010 and its amendments).

**Open:** only what needs hardware this project does not have here: two
machines on different networks (through n0's relays, and through
`server/compose`'s relay exposed to both), and a real phone with background
sync on for a day (battery, and what its maker's power manager does to it).

## Encrypted backups: what is left

Built (ADR 0020, `docs/handoff.md`). **Open:**
- SFTP on Android (libssh against Qt's OpenSSL, or a different SSH library).
- Google Drive and OneDrive against the real services, once the OAuth clients
  exist (ADR 0019).
- At-rest encryption of the local database (SQLCipher), the rest of P11.

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
