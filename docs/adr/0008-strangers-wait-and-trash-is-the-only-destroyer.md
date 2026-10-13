# 0008 — Strangers' mail waits in Unverified; Trash is the only thing that destroys

**Status:** Accepted — 2026-10-05 · carries over eeemail ADRs 0018, 0019, 0027

- Mail from a sender who is neither verified nor known (we wrote to them, or
  the user accepted them) waits in **Unverified**. Writing to someone or
  accepting them releases their mail. The window is read from the setting at
  sweep time, not stored, and 0 means never sweep.
- Past the window it moves to **Trash** with reason "unaccepted". Restoring it
  accepts the sender.
- A **blocklist** entry (an address, or exactly `@domain` without subdomains)
  trashes mail on arrival.
- **Trash** destroys after `trash_purge_days`, where 0 means at the next
  housekeeping. It is the only place mail is destroyed on a timer, and the
  reading pane says why each message is there.

The two zeros mean opposite things deliberately: someone who wants strangers'
mail gone at once turns gating off.

## Amendment — 2026-10-13: writing to someone puts them in the address book

- **Sending puts every To, Cc and Bcc recipient in the address book** (origin
  Manual), as accepting them does, and records `contact.accept` for each one
  not already there. Before, writing made them known only on the device that
  sent (origin Outgoing, no op): on the other devices they stayed strangers,
  and their replies waited in Unverified there.
- Our own address is never added. A recipient already in the book records no
  new op, so writing to someone again costs nothing.
- It does not make anyone **verified**. Verified means a key checked in
  person, which pins it against replacement; an address typed into To says
  nothing about a key.
