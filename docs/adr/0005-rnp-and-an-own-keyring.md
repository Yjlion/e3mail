# 0005 — OpenPGP through RNP, in a keyring of our own

**Status:** Accepted — 2026-10-05

## Decision
RNP (Thunderbird's OpenPGP library, Botan backend), embedded. Each account's
keys live in its database and are loaded into an in-memory RNP keyring per
thread. The user's GnuPG keyring is never touched. Each account gets an
Ed25519 key with a Curve25519 encryption subkey and no expiry. The private key
is not passphrase-protected, because the database holding it is what gets
protected (P11).

## Why not GnuPG or Sequoia
GPGME needs a gpg installation and agent on every platform, and shares state
with the user's own keyring. Sequoia would add a Rust toolchain to every build
for the core path. RNP is a C library that builds on all three platforms.

## Consequences
- RNP has no vcpkg port; `scripts/build-rnp.sh` builds it where no package
  exists.
- A second implementation reads what we write: a test runs `gpg --decrypt` on
  our PGP/MIME and expects `DECRYPTION_OKAY` and `VALIDSIG`.
