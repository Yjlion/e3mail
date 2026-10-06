# 0004 — Opportunistic encryption is all or nothing per message

**Status:** Accepted — 2026-10-05 · revises eeemail ADR 0006's opportunistic mode

## Context
In eeemail, an encrypted message went only to the recipients whose key was
known. The others stayed in the `To` header but were silently dropped from the
envelope. eeemail kept this because upstream's tests depended on it, and
recorded the drop afterwards. A new engine has no such constraint.

## Decision
- **Opportunistic** (default): encrypt when every recipient has a usable key;
  otherwise send cleartext to all. This is Autocrypt Level 1's recommendation.
- **Strict**: encrypt, or refuse to send and name who has no key. Incoming
  cleartext goes to Trash, recoverably, with the reason shown.
- **Lenient**: encrypt only when every recipient has a key *and* asked for
  encryption (`prefer-encrypt=mutual`).
- Per-contact overrides and the account mode compose toward the strictest.
- The composer's padlock decides per message. It can demand encryption, which
  refuses when a key is missing. It can ask for cleartext, which strict mode
  refuses. The composer shows the outcome while the user types.

## Consequences
- No recipient ever silently misses a message for want of a key. The
  `undelivered` table now records only what the SMTP server refused.
- One keyless recipient means the whole message goes in cleartext, and the
  composer says so before sending.
- Bcc recipients' key IDs appear in the encrypted message, so anyone who
  inspects it can see that Bcc recipients exist. This is a known gap.
