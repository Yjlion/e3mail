# 0006 — IMAP and POP3 are transport only; retention is never retroactive

**Status:** Accepted — 2026-10-05

## Decision
The local database is the mailbox. IMAP reads one folder (INBOX) by UID and
waits with IDLE. POP3 reads by UIDL and polls. Neither ever writes content
back. Server retention is a setting:

- **delete after download** (default);
- **keep N days**;
- **never**, so another client can keep using the mailbox.

Messages present the first time e3mail looks at a mailbox are marked
pre-existing and never deleted. IMAP deletion uses `UID EXPUNGE` when the
server offers UIDPLUS, so it never expunges what another client marked.

## Consequences
- POP3 suits this model naturally: download, then `DELE` at `QUIT`, and a
  dropped session rolls the deletions back.
- With several devices (ADR 0010), "after download" means "after every device
  has it".
