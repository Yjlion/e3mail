# 0012 — Passwords go in the OS keyring, and the fallback says so

**Status:** Accepted — 2026-10-05

Mail passwords are stored through QtKeychain: Keychain on macOS, Credential
Manager on Windows, Secret Service on Linux. When none is available (a headless
machine, or a build without QtKeychain), they go in the account database, and
Settings says that is where they are. The setup form never fails merely
because no keyring exists. When the database gets a passphrase (P11), this
fallback stops being cleartext.
