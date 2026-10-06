# 0001 — Write our own engine in C++ on Qt, without Delta Chat's core

**Status:** Accepted — 2026-10-05

## Context
eeemail forked `chatmail/core` and built an email client on a chat engine. It
worked, but a large share of its effort went into bending chat semantics to
email: recipients came from chat membership, there was no `Cc` header, upstream
defaults had to be overridden at setup to avoid permanent merge conflicts, and
every touch of an upstream file needed a ledger entry. Its desktop app reached
that Rust engine over an IPC path that no test exercised. The user asked for a
clean port to Qt, multi-platform like VLC, without the Delta Chat backend, with
POP3.

## Decision
A new engine, `libe3core`, in C++20 on QtCore and QtNetwork. It has no GUI
dependency and is called in-process by the app, the CLI and the tests. The
model is email-native from the first migration.

## Consequences
- Autocrypt, PGP/MIME and (later) SecureJoin are ours to implement and to get
  right. Interop is a test obligation, not inherited: GnuPG interop is tested
  today, and Delta Chat interop is not yet.
- One language and one build system from the engine to the installer. Rust
  enters only as an optional, contained crate for Iroh (ADR 0010).
- POP3 is a first-class receive path.
