# 0021 — SecureJoin: verifying a contact by scanning their code

**Status:** Accepted — 2026-10-21

## Context
Autocrypt gets keys to people without asking anything of them, but it cannot
tell a real key from one a server or a man in the middle put there. Contacts
had a `verified` column from the first schema and a `contact.verify` op from
P8, and nothing that set them. Delta Chat solves this with SecureJoin
(countermitm 0.10): one person shows a QR code holding their fingerprint and
two random tokens; the other scans it, and four mails later each holds the
other's key verified. Speaking that same protocol lets e3mail verify against
Delta Chat as well as against itself.

Delta Chat 2.x (checked against chatmail/core v2.64.0) has two variants. The
newer one (`v=3` in the code) opens with a message encrypted to a password
derived from the token, as SEIPDv2 with AES-128-OCB. RNP 0.18 reads and writes
SEIPDv2 only when built with its experimental crypto-refresh support, which
neither the distributions nor our builds enable. The older variant is still
handled by the inviter's side of the same release.

## Decision
- **The setup-contact protocol only, the older variant.** The steps are
  `vc-request`, `vc-auth-required`, `vc-request-with-auth` and
  `vc-contact-confirm`, with the `Secure-Join-*` headers Delta Chat uses.
  - e3mail's invites are `OPENPGP4FPR:<fpr>#a=<addr>&n=<name>&i=<number>&s=<token>`,
    without `v=3`, so Delta Chat answers them the older way.
  - Scanning a Delta Chat invite also takes the older way.
  - Group invites (`g=`, `x=`, `b=`) are refused: e3mail has no groups.
- **What verifies whom:**
  - **The joiner** verifies the inviter when an encrypted answer is signed
    by the key the code named.
  - **The inviter** verifies the joiner when the token comes back encrypted,
    signed by the key the joiner names in `Secure-Join-Fingerprint`.
  - Every step after the first must also gossip the recipient's own key back
    to them, which is Delta Chat's check against surreptitious forwarding.
  - A joiner who already holds the named key skips to sending the token.
- **The token never leaves encryption.** The first step is cleartext, since
  the joiner has no key for the inviter yet, and it carries only the invite
  number. The token and the joiner's fingerprint travel in protected
  headers inside the encrypted part (`OutgoingMail::innerHeaders`).
- **Handshake mail is hidden:**
  - It is stored with `hidden = 1`, like device-sync mail.
  - It is never shown, held, announced or recorded as `msg.add`.
  - It is deleted from the server under the usual retention.
  - It is not answered when its Date is more than a week old, so a device
    that fetches it late starts nothing.
- **An invite stays valid until renewed.** The same code is shown each time,
  so it can be printed. "New code" revokes every earlier one.
- **A join is given up after a week** without an answer, by housekeeping.
- **Devices.**
  - Invites and joins replicate as `sj.invite` (keyed per invite number,
    last writer wins, which is how a revocation spreads) and `sj.join`
    (keyed per inviter address).
  - Every device fetches every message, so any device can answer. Two
    devices may both answer the same step. The peer ignores a step it no
    longer expects, and a second verification of the same key changes
    nothing, so the duplicate is harmless and not worth a lock.
- **Verifying releases held mail.** A verified contact is trusted (ADR 0008),
  so their mail waiting in Unverified moves to the Inbox. This applies on
  every device, as `contact.verify` arrives.

## Consequences
- e3mail interoperates with Delta Chat only as long as Delta Chat keeps
  answering the older variant. When RNP supports SEIPDv2 by default, add the
  newer one (`vc-request-pubkey` / `vc-pubkey`) alongside.
- Whoever sees a code can verify themselves to its owner as themselves. They
  cannot verify as someone else. Anyone who can see the code and also swap
  the one the other person scans can do more, as in any QR-based scheme.
  "New code" is the remedy for a code that was seen by the wrong person.
- An invite carries the address and display name in clear, like any QR code
  one shows.
- A step answered by two devices sends two messages.
