# 0013 — How devices merge the op-log

**Status:** Accepted — 2026-10-06

## Context
ADR 0010 settles that devices exchange a replicated op-log, but not what an op
means when it arrives, in which order, or before the message it names. Those
rules have to hold without a transport, so they are decided and tested before
Iroh joins the build.

## Decision
- **The log decides.** Every op carries a merge key computed from its kind and
  payload by `OpLog::mergeKey`, on every device, never trusted from the wire.
  A remote op takes effect only if no op already in the log with the same key
  has a higher `(hlc, device)`. There are no per-field clock columns.
  - `msg.read` is keyed by Message-ID; `msg.trash` and `msg.restore` share one
    key per Message-ID.
  - `label.add` and `label.remove` share one key per (Message-ID, label name):
    a tag set is a last-writer-wins flag per tag. `label.define`,
    `label.rename` and `label.delete` share one key per label name.
  - `block.add`/`block.remove` per pattern; `contact.edit`, `contact.policy`
    and `contact.verify` per address; `config.set` per setting.
  - `msg.add`, `contact.accept`, `key.learned` and `server.ack` have no key:
    they only ever add.
- **Purge is a tombstone** and beats everything for its Message-ID, including
  a later re-download from the server. Trash is the only destroyer
  (ADR 0008), and a purge on any device is final on all of them.
- **Deferred ops.** An op for a message this device does not have yet stays in
  the log. Whenever a message is stored, by any path, the newest op of each of
  its keys is applied.
- **Placement on arrival is derived, not recorded.** Trash for blocked senders
  or cleartext under strict mode, the Unverified hold, and the sweep of
  unaccepted mail follow from replicated inputs (blocklist, settings,
  contacts), so each device computes them. Recording them would stamp a fresh
  clock on a machine decision and let it overwrite what a person did on another
  device.
- **Synced settings** are the account's behaviour: display name, signature,
  encryption mode, server retention, trash and Unverified windows, gating.
  Raw-message retention and polling stay per device; server settings and
  secrets never travel as ops.
- **Anti-entropy.** A peer says which ops it has as `{device: highest seq}`
  and is sent the rest, oldest first. Ops are accepted only in sequence per
  device; a gap stops that device's ops until the next exchange. Ops relay:
  a device passes on other devices' ops too.
- **Server deletion** waits until this device and every paired device have
  acknowledged the server key (`server.ack`, keyed `imap:<validity>:<uid>` or
  `pop3:<uidl>`, which are the same on every device). With no paired devices
  this is the single-device rule unchanged.
- A device's clock observes the highest clock in its log before every local
  op, so all connections to one database share one causal order.

## Consequences
- Label rename is keyed by the old name. A device that tags with the old name
  before it hears of the rename brings the old label back.
- `msg.add` from a peer needs the raw message as a blob. If it has expired
  under raw-message retention, the message reaches that device only from the
  server, and sent mail never does.
- A paired device that is never online, directly or through the mailbox
  fallback, holds every message on the server.

## Amendment — 2026-10-07: the address book (P10)

- **`contact.details`** carries a contact's organization, title, notes,
  birthday and phone numbers as one record, keyed `cdetail:<addr>`: last
  writer wins for the whole record, not field by field. The name stays in
  `contact.edit`, keyed as before.
- **`contact.remove`** (key `cremove:<addr>`) competes with every other op
  about that address, not only with its own key: `contact.accept`, `.edit`,
  `.policy`, `.verify` and `.details`. A removal takes effect only if none of
  those is newer by `(hlc, device)`, and none of those takes effect if a
  removal is newer. So an edit made after a removal on another device keeps
  the contact, and an older edit arriving late does not bring a removed
  contact back.
- Contacts that Ingest creates from arriving mail are derived, as before, and
  never recorded: a removed contact whose mail arrives again is a contact
  only *seen*, not one in the book, so its mail waits in Unverified.
- Keys never travel in a contact. vCard import ignores `KEY`, and export does
  not write it: keys come only from Autocrypt and verification.
