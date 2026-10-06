# 0007 — Tags, not folders; system tags derived where possible

**Status:** Accepted — 2026-10-05 · carries over eeemail ADRs 0005, 0009, 0017

Inbox, Sent and Drafts are derived from direction, state and the absence of
other tags. Archive is a reserved label row. Trash and Unverified have tables of
their own because each carries a deadline. User tags are many-to-many. Label
identity is case-insensitive (`name_norm`). Every list view is one
`SearchQuery`, and search spans subject, body and people through SQLite FTS5.
