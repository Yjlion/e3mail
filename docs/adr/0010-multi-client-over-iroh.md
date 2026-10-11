# 0010 — Several devices sync with each other over Iroh; the server stays a spool

**Status:** Accepted — 2026-10-05 · implementation is Phase 8

## Context
A local mailbox fed from a spool means a second device sees nothing the first
already took. eeemail's answer, Delta Chat's sync messages to self over IMAP,
fails under delete-after-download and cannot work over POP3.

## Decision
- Device-to-device transport over **Iroh**: dial by public key, QUIC with hole
  punching and relay fallback, and `iroh-blobs` for verified bulk transfer.
- One **small Rust crate** (`src/p2p/`) exposing a narrow non-blocking C ABI,
  built with Corrosion. Iroh's official C binding was evaluated: it is itself
  built with cargo, so it would not remove Rust from the build, and it lacks
  `iroh-blobs`.
- A **replicated op-log**, in the schema from migration 1 so sync is never
  retrofitted. HLC-stamped and keyed by Message-ID, never by local id.
- **Server deletion waits for every paired device's acknowledgement**. This
  works for IMAP and POP3 alike, and the server bridges devices that are never
  online together.
- A **mailbox fallback** carries ops when no peer is reachable.

## Consequences
- A Rust toolchain joins the build in Phase 8, contained to one crate.
- libdatachannel (C++, MPL-2.0) remains the fallback behind the same
  `DeviceTransport` interface if that cost proves too high.

## Amendment — 2026-10-11: the transport as built

- **iroh 1.x, without iroh-blobs.** iroh reached 1.0; iroh-blobs is still
  0.x and follows it at a distance. Raw messages are small enough to travel
  as frames on the sync connection, so the crate depends on iroh alone.
- **One QUIC connection per pair of devices**, ALPN `e3mail/sync/1`, one
  bidirectional stream carrying frames `[kind: u8][length: u32 BE][bytes]`,
  at most 64 MiB each. Kinds: 1 hello `{"device": id}`, 2 a SyncSession
  message, 3 blob request, 4 blob (64-byte name, then data), 5 blob missing;
  16 and up belong to pairing.
- **Who may connect.** Each device has its own Iroh key (`p2p_secret`, never
  synced or copied) and records the other devices' endpoint ids
  (`devices.node_id`, migration 4). The crate refuses any other endpoint
  before reading a byte. A hello naming a device that is not the one paired
  with that endpoint is ignored. When two devices dial each other at once,
  both keep the connection dialed by the endpoint with the smaller id.
- **Finding each other.** A device dials the address a peer was last seen at
  (`devices.addr`), or its id alone, which n0's address lookup resolves.
  Relays: n0's public ones by default, a URL the person runs, or none
  (`p2p_relay`: `default`, a URL, `off`; per device). Relays and lookup only
  ever see encrypted QUIC.
- **The C ABI** is non-blocking (`src/p2p/e3p2p.h`): calls return at once,
  events are polled after a waker, which `IrohTransport` turns into a queued
  call on its own thread.
- `E3MAIL_P2P=OFF` builds without Rust and without device sync.

## Amendment — 2026-10-11: pairing

- **The code.** The device that has the account makes a one-time code,
  valid ten minutes: `E3PAIR1:` and base64url of its endpoint address, a
  random 128-bit token, the account's address and its relay setting. It is
  shown as a QR code and as text; the new device pastes it (camera scanning
  is not built). While the code is out, the endpoint lets any peer connect.
- **A person allows it.** The new device sends the token and a name; the
  existing device asks "“Laptop” wants to become a device of this account"
  and sends nothing until someone allows it. Holding the code is not enough,
  and a code serves one device.
- **The copy is pulled.** The new device asks for the database (a
  `VACUUM INTO` copy without this device's own settings) and every blob in
  1 MiB pieces, eight at a time, so a large mailbox is never in memory on
  either side. The database is checked against the SHA-256 in the manifest,
  each blob against its name. The passwords travel in the manifest, inside
  the authenticated QUIC connection, and go to the new device's keyring.
- **The new device** gets its own device id and Iroh key, records the
  existing device as its first peer, and records a `server.ack` for every
  server message the copy holds, so the others do not keep them on the
  server waiting for it. The existing device records it, with a
  `device.pair` op that tells every other device (ADR 0013 amendment).
- Frames 16 and up are pairing's: request, denied, manifest, get, chunk,
  done.
