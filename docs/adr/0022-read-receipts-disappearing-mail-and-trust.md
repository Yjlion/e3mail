# 0022 — Read receipts, disappearing mail, and trust set by hand

**Status:** Accepted — 2026-10-21

## Context
Phase 12 asks for two things that both tell the sender something after their
mail arrives: read receipts (RFC 8098) and disappearing messages. `TrashReason`
has had an `Expired` value for this since the first schema, and nothing used it.

A receipt tells its recipient that a person opened a message, and when.
Answering every request would let anyone who writes to you confirm that the
address is read. Delta Chat answers every request automatically. Thunderbird
asks the person each time. The person chose this default: send to trusted
senders, with "never" as the alternative. "Trusted" until now meant only
being in the address book or verified. The person also asked for a per-contact
"Trusted" switch, so trust can be given or taken away by hand.

## Decision
- **Trust is one notion, with an override.** `contacts.trust` is NULL (the
  rule: in the address book, or verified), 1, or 0.
  - `Contacts::isTrusted` honours the override first; our own address is
    always trusted.
  - The override decides everywhere trust is asked: the Unverified hold,
    read receipts, and later the buttons on structured cards (P13).
  - Trusting someone releases their held mail.
  - It syncs as `contact.trust`, keyed per address, last writer wins.
- **Asking for a receipt:**
  - The composer can ask, with `Disposition-Notification-To`.
  - When the message is encrypted, the header goes only inside the
    encryption, with the protected headers, so the server does not learn it.
- **Answering a request:**
  - A receipt goes once, when the person first marks the message read (in
    the app, opening it).
  - All of these must hold:
    - the setting `mdn_send` is `trusted` (synced; `never` turns receipts off);
    - the sender is trusted;
    - the message is neither held nor in Trash;
    - the address asked for is the sender's own (RFC 8098 §2.1: a receipt
      elsewhere tells a third party);
    - no device has sent one for this message yet.
  - That last condition is the op `mdn.sent`, keyed per Message-ID, recorded
    in the same transaction. A device reading the message after the op
    arrived sends nothing. Two devices reading before either hears of the
    other may each send one. The second only repeats what the first said.
  - **The receipt itself:**
    - It is a hidden `multipart/report`, never listed or synced as mail.
    - Its fixed English text says nothing of the reader's language.
    - It is encrypted when the message was, and never sent in cleartext for
      encrypted mail.
  - Delta Chat's `Chat-Disposition-Notification-To` is honoured the same way.
- **Receipts that come back:**
  - Each one is recorded per reader in `receipts`, but only when the report
    comes from someone the message went to.
  - Every device fetches the report itself, so this is derived and needs no
    op.
  - The message shows "read by …".
- **Disappearing mail:** Delta Chat's `Ephemeral-Timer: <seconds>`, per
  message, chosen in the composer (1 hour, 1 day, 1 week, 4 weeks).
  - **When the clock starts:** for mail we receive, when it is first read.
    `msg.read` already travels, and a device applying it starts the clock
    from the op's time. Our own copy starts at sending.
  - **Expiry:** an expired message goes to Trash with reason `Expired`, as
    `msg.trash`, and is destroyed on the Trash schedule (ADR 0008: Trash is
    the only destroyer).
  - **Who checks:** the worker checks every pass, not only the hourly
    housekeeping, and the app sets a timer for the next one.
  - **Limits:** a lifetime longer than a year, or anything unreadable,
    counts as none.
- **CLI:** `send --receipt --disappear <s>`, `read <id>` (as opening the
  message does), and `contact-trust <addr> on|off|auto`.

## Consequences
- An expired message is gone from the lists at once, but stays in Trash for
  the Trash period. Someone who needs it gone sooner empties Trash.
- Nothing makes the recipient's other clients honour the timer, or keeps
  anyone from copying a message. The composer offers it as a courtesy, not
  as a guarantee.
- Taking trust away from someone in the address book holds their next mail
  in Unverified. The contact's switch explains this.
- A receipt to a sender with no usable key, for encrypted mail, is skipped
  rather than sent in cleartext.
