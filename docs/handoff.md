# Handoff — what is true right now

Updated with what each piece of work taught, not with what was planned. It is
blunt about gaps on purpose; keep it that way.

## State (v0.1.0, 2026-10-05)

Phases 0–7 of [DESIGN.md](DESIGN.md) are done: the engine, the desktop app, the
CLI and packaging.

**Verified here (Arch Linux, Qt 6.11, RNP 0.18.1, GCC 16):**
- `ctest --preset dev`: 6 suites, all passing. They cover MIME, the
  sanitizer, the protocols against the fake mail system, crypto (including
  GnuPG decrypting and verifying our output), an engine end-to-end
  conversation over IMAP and POP3, the composer's whitelist, and a boot of the
  real QML app.
- `scripts/e2e.py` against `server/compose`, all green:
  - real TLS on every port, implicit and STARTTLS;
  - alice on IMAP, bob on POP3, carol on IMAP with STARTTLS, dana on POP3 with
    STARTTLS;
  - delete-after-download observed emptying the server, and pre-existing mail
    left alone.
- The AppImage builds and boots offscreen. The `.deb` builds (on Arch, without
  `dpkg-shlibdeps`).
- Every view rendered and inspected (`scripts/screenshots.sh`).

**Not verified:**
- The Windows and macOS CI and release jobs have never run. Expect them to need
  fixes. The Windows RNP build in particular (`scripts/build-rnp.sh` with
  vcpkg's Botan, MSVC) is untested.
- The app has not been used interactively on a real display. Offscreen
  rendering and the smoke test are all there is.
- No interop with Delta Chat, Thunderbird, Gmail or any mainstream provider.
  OAuth2 is not supported, so Gmail and Outlook need app passwords.

## Known gaps

- **Signed-only mail** (`multipart/signed` without encryption) is shown, but
  its signature is not checked, so it reads as unsigned.
- **Inline PGP** (`-----BEGIN PGP MESSAGE-----` in a text body) is not
  decrypted.
- **Bcc and encryption**: Bcc recipients' key IDs are in the encrypted message,
  so recipients who inspect it can tell that Bcc recipients exist
  (ADR 0004).
- **First sync of a large mailbox** downloads everything in one pass with
  `UID SEARCH ALL`. There is no paging and no limit.
- **New-mail notifications**: `MailApp::newMailArrived` is emitted, but nothing
  shows a desktop notification yet.
- **Contacts** cannot be added by hand, and there are no address-book fields
  yet (P10).
- **Replies** quote the plain-text body, even of HTML mail.
- **Charsets** beyond UTF-8, Latin-1 and Windows-1252 rely on the Qt build
  having ICU.
- **No translations**, and plurals are hand-written English.
- **Passwords fall back to the database in cleartext** when there is no OS
  keyring, and Settings says so (ADR 0012). The database has no passphrase
  until P11.

## Traps

**1. `Statement::bind(QString)` always binds text, never NULL.** A
default-constructed `QString` used to bind NULL and broke NOT NULL columns. Use
`nullIfEmpty(s)` or `std::optional` when NULL is meant; nullable columns such
as `raw_blob` are tested with `IS NULL`.

**2. Outgoing dates are whole seconds.** The `Date:` header has seconds
precision, and a stored date in milliseconds sorted a reply *before* its
parent. `Compose::queue` truncates.

**3. Deadlines are computed in UTC.** `QDateTime::addDays` in local time across
a DST change adds an hour, which showed "31 days" for a 30-day window.

**4. A first encrypted message carries the key that signed it.** Ingest
verifies again after learning keys. Without that, the first encrypted reply
from anyone reads as unsigned.

**5. Docker Compose project names.** Left to default, the test server's project
was named after its directory, `compose`, which eeemail's test server shares.
Bringing it up replaced the `eeemail-mail` container on this machine (since
restored from its own compose file). The project is now named `e3mail-test`.
Never drop the `name:` line.

**6. AppImage.** linuxdeploy's bundled `strip` fails on newer ELF sections, so
the script sets `NO_STRIP=1`. Only `xcb` is deployed by default, so `wayland`
and `offscreen` are added explicitly. Without `offscreen` the AppImage aborts
silently under `QT_QPA_PLATFORM=offscreen`.

**7. `QLatin1String` with non-ASCII text** (like `•`) compares garbage, and
makes a passing test fail. Use `QStringLiteral`.

**8. RNP's CMake package** names `JSON-C::JSON-C` and `Botan::Botan` without
creating them. `cmake/E3mailRnp.cmake` provides them, and falls back to
pkg-config.

## Next

1. Get CI green on Windows and macOS (P7 follow-through).
2. P8 multi-client over Iroh, per [ADR 0010](adr/0010-multi-client-over-iroh.md).
3. P9 SecureJoin, then an interop pass against Delta Chat's released
   `deltachat-rpc-server`, as eeemail's `scripts/interop-pass.py` does.
4. Verify signed-only mail; desktop notifications.
