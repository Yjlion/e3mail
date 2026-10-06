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
