# 0007 — Tags, not folders; system tags derived where possible

**Status:** Accepted — 2026-10-05 · carries over eeemail ADRs 0005, 0009, 0017

Inbox, Sent and Drafts are derived from direction, state and the absence of
other tags. Archive is a reserved label row. Trash and Unverified have tables of
their own because each carries a deadline. User tags are many-to-many. Label
identity is case-insensitive (`name_norm`). Every list view is one
`SearchQuery`, and search spans subject, body and people through SQLite FTS5.

## Amendment — 2026-10-08: mail to oneself is in the Inbox too

Sent mail with the account's own address among its recipients (To, Cc or
Bcc) is in the Inbox as well as in Sent, as it is in other mail programs:
it was delivered to us. It stays derived. The Inbox clause adds sent
messages (state Sent) with a recipient equal to the account's address, so
there is no stored flag, no migration and no op, and every device agrees.
It is one message (Message-ID is unique), so Archive takes it out of the
Inbox only and Trash takes it out of both. It shows read, because we wrote
it.

**All mail** (`tag::All`, which searching from the Inbox already used) gets
its own place in the sidebar: everything not in Trash, Unverified or Drafts,
archived mail included.
