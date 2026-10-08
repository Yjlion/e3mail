# Architecture Decision Records

One decision per file. Accepted ADRs are immutable: to change one, add an ADR
that supersedes it, or a dated amendment block inside it, and add a row here.

| # | Decision | Status |
|---|---|---|
| [0001](0001-own-engine-in-cpp-on-qt.md) | Our own engine in C++ on Qt, without Delta Chat's core | Accepted |
| [0002](0002-license-mpl-2.0.md) | MPL-2.0 | Accepted |
| [0003](0003-qml-and-no-web-engine.md) | Qt Quick UI; mail never renders in a web engine | Accepted |
| [0004](0004-all-or-nothing-encryption.md) | Opportunistic encryption is all or nothing per message | Accepted |
| [0005](0005-rnp-and-an-own-keyring.md) | OpenPGP through RNP, in a keyring of our own | Accepted |
| [0006](0006-transport-only-imap-and-pop3.md) | IMAP and POP3 are transport only; retention is not retroactive | Accepted |
| [0007](0007-tags-not-folders.md) | Tags, not folders | Accepted |
| [0008](0008-strangers-wait-and-trash-is-the-only-destroyer.md) | Strangers wait in Unverified; Trash is the only destroyer | Accepted |
| [0009](0009-two-barriers-for-message-content.md) | Two barriers between message content and the network | Accepted; amended 2026-10-07 (inline images) |
| [0010](0010-multi-client-over-iroh.md) | Devices sync with each other over Iroh | Accepted, Phase 8 |
| [0011](0011-linear-protocol-code-over-an-event-loop.md) | Linear protocol code over a local event loop | Accepted |
| [0012](0012-passwords-in-the-os-keyring.md) | Passwords in the OS keyring, with an honest fallback | Accepted |
| [0013](0013-op-merge-rules.md) | How devices merge the op-log | Accepted, Phase 8; amended 2026-10-07 (address book) |
| [0014](0014-languages-icu-and-qt-linguist.md) | Languages: ICU for charsets and dates, Qt Linguist for the interface | Accepted |
| [0015](0015-finding-server-settings.md) | Finding server settings: provider, ISPDB, Autodiscover, DNS, guess | Accepted |
