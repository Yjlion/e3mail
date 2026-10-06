# 0011 — Protocol code reads linearly; the socket spins a local event loop

**Status:** Accepted — 2026-10-05

SMTP, IMAP and POP3 are conversations, and written as callback state machines
they are hard to read and harder to review. They run on worker threads over
`net::SyncSocket`, whose reads wait by spinning a local `QEventLoop`. This
avoids `QAbstractSocket::waitFor*`, which Qt documents as unreliable on
Windows. A wait can be interrupted between ticks, which is how IMAP IDLE and
POP3 polling wake for an outgoing message. The same property lets the fake
mail server in the tests run on the test's own thread.
