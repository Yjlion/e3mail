# Handoff — what is true right now

Updated with what each piece of work taught, not with what was planned. It is
blunt about gaps on purpose; keep it that way.

## State (v0.1.0, 2026-10-05)

Phases 0–7 of [DESIGN.md](DESIGN.md) are done: the engine, the desktop app, the
CLI and packaging.

**Verified here (Arch Linux, Qt 6.11, RNP 0.18.1, GCC 16):**
- `ctest --preset dev`: 9 tests, all passing. They cover MIME and charsets,
  the sanitizer, the protocols against the fake mail system, crypto (including
  GnuPG decrypting and verifying our output), an engine end-to-end
  conversation over IMAP and POP3, two devices of one account syncing, the
  composer's whitelist, the translations, and a boot of the real QML app,
  left to right and right to left.
- `scripts/e2e.py` against `server/compose`, all green:
  - real TLS on every port, implicit and STARTTLS;
  - alice on IMAP, bob on POP3, carol on IMAP with STARTTLS, dana on POP3 with
    STARTTLS;
  - delete-after-download observed emptying the server, and pre-existing mail
    left alone.
- The AppImage builds and boots offscreen. The `.deb` builds (on Arch, without
  `dpkg-shlibdeps`).
- Every view rendered and inspected (`scripts/screenshots.sh`).

**Verified by CI (GitHub Actions, PR #1):** Linux (Ubuntu 24.04, distribution
RNP on Botan 2), macOS 14 (Homebrew RNP, clang) and Windows (MSVC 2022, vcpkg,
RNP built from source) all build with warnings as errors and pass all six
suites, including the GnuPG interop test against Git for Windows' gpg.

**Not verified:**
- The release workflow (installers, portable zip, `.dmg`) has never run. Expect
  it to need fixes like the CI did.
- The app has not been used interactively on a real display. Offscreen
  rendering and the smoke test are all there is.
- No interop with Delta Chat, Thunderbird, Gmail or any mainstream provider.
  OAuth2 is not supported, so Gmail and Outlook need app passwords.

## P8 in progress: the sync core (2026-10-06)

The transport-independent half of multi-client is in, without Iroh: this
machine has no Rust toolchain, and the merge rules had to be right before
bytes move. [ADR 0013](adr/0013-op-merge-rules.md) has the rules.

- Migration 2: merge keys on `ops`, `tombstones`, `devices`, `server_acks`.
- `src/core/sync/`: `OpApply` (merge, tombstones, deferred ops),
  `SyncSession` (anti-entropy over a `DeviceTransport`), `DeviceTransport`
  (the interface Iroh will sit behind).
- `mail::Preferences::set` is now the way to change a setting; synced ones
  are recorded. The app and the CLI use it.
- Server deletion waits for a `server.ack` from every paired device.
- `tst_sync` runs two devices of one account against the fake server over
  `tests/LoopbackTransport`. Each mechanism (replay, LWW, the device wait,
  gap refill) was broken on purpose once to see the suite fail.

`scripts/e2e.py` against `server/compose` passes with this change, so
single-device delete-after-download is unchanged on real servers.

**Not built yet:** the Rust crate and Corrosion, pairing (QR and the
encrypted snapshot; tests clone the account directory instead), the mailbox
fallback, any UI, and wiring a `SyncSession` into `Account`. Nothing in the
app syncs yet.

## Languages and ICU (2026-10-06)

ICU is linked by the core, and the interface is in eleven languages
([ADR 0014](adr/0014-languages-icu-and-qt-linguist.md),
[`src/app/translations/`](../src/app/translations/README.md)).

**Verified here:**
- `tst_mime` decodes seventeen charsets (ISO-2022-JP, Shift_JIS, EUC-JP,
  GB2312/GBK/GB18030, Big5, EUC-KR, KOI8-R, windows-1250/1251/1255/1256,
  ISO-8859-2/6/8/15) through ICU, mislabelled ones included, and builds the
  day-and-month date pattern from CLDR.
- The `translations` test: all ten files complete, placeholders intact.
  `app_smoke_rtl` boots the app in Arabic.
- Screenshots in every language were looked at, at least the inbox, reading
  pane or settings; Arabic and Hebrew also contacts, first run and the
  Unverified card; Russian the composer. A live switch from English to Hebrew
  and German was watched by a temporary hook, not by a test.

**Verified by CI** (run 37497307631): Linux, macOS and Windows build with ICU
and pass every test, translations and the right-to-left boot included.
Windows took 40 minutes, almost all of it vcpkg building ICU; the dependency
cache holds it from then on.

**Not verified:** the packages carrying ICU (the release workflow has still
never run), any of it on a real display, or by a native speaker.

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
- **Sync**: a message another device sent while it was still pending in that
  device's outbox arrives as Sent. Mail whose raw copy expired under
  raw-message retention never reaches a new device unless the server still
  has it, and sent mail then never does. A label renamed on one device comes
  back if another tags with the old name first (ADR 0013).
- **Translations are machine-made** and unreviewed in all ten languages, and
  Settings says so. Yiddish is the least certain of them.
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

**8. Getting CI green on Windows took nine rounds**, each a trap of its own:
vcpkg's manifest mode refuses per-package installs; RNP always builds its CLI
tools, which need `getopt-win32` and `dirent`; setting `CMAKE_CXX_FLAGS`
replaces MSVC's defaults, `/EHsc` among them (RNP was briefly built without
exception unwinding); there is no pkg-config, so Botan is found by hand; Qt's
headers trip C4702 under `/WX`; and Git for Windows' MSYS gpg reads `C:\...` as
a relative path. `actions/cache` saves only after a successful job, so the
dependency cache did not exist until the first green run. When a Windows test
dies with no output, `scripts/ci-diagnose-tests.sh` (run by CI on failure)
uses QtTest's file logger, because its stdout arrived empty.

**9. RNP's CMake package** names `JSON-C::JSON-C` and `Botan::Botan` without
creating them. `cmake/E3mailRnp.cmake` provides them, and falls back to
pkg-config.

**10. Clocks do not fit in JSON numbers.** An HLC is 64 bits and JSON numbers
are doubles, exact only to 2^53. On the wire the clock is a string.

**11. Placement on arrival must not be recorded.** Auto-trash (blocked,
cleartext under strict) used to record `msg.trash`. Replayed on another
device, that machine decision got a fresh clock and overwrote a Restore the
person had made elsewhere. Only what a person or a timer does is an op.

**12. Two connections write ops.** The UI and the worker each have an
`OpLog`. Sequence numbers are taken inside the insert, and the clock observes
the log's highest value before every op. Otherwise the worker's frequent
`server.ack` ops race the UI's on `UNIQUE(device, seq)`.

**13. `e2e.py` leaves one message on dana's server on purpose** (the
pre-existing one, which retention must never delete). A second run against
the same container then counts two and fails "it stays on the server".
Empty `/var/mail/e3.test/dana/Maildir/{new,cur}` in `e3mail-test-mail`
between runs.

**14. Retranslating rebuilds translated ComboBox models**, and a plain
ComboBox then shows its first entry: changing the language showed the
encryption mode as Lenient. Use `ChoiceBox` and bind `selected`.

**15. Read RTL screenshots zoomed in.** At full size, an English subject in a
Hebrew window looked like "!You have been selected", and three rounds of bidi
fixes (isolates, marks, embeddings) went at a bug that did not exist. Cropped
and enlarged it read correctly. The real problem was only that mail text
aligned by its own script sat at the far edge; it now hugs its text so the
mirrored row places it.

**16. Short labels grow in translation.** The Russian "PREVIEW" badge pushed
the sidebar wider than the window allowed. Badges elide now; keep such
translations short.

## Next

1. P8, slice 2: install Rust, build `src/p2p/` (Iroh, `iroh-blobs`) with
   Corrosion behind `DeviceTransport`, add the toolchain to CI on all three
   platforms, and wire `SyncSession` into `Account`.
2. P8, slice 3: QR pairing with the encrypted snapshot; the mailbox fallback.
3. Run the release workflow once (`workflow_dispatch`); it has never run.
4. P9 SecureJoin, then an interop pass against Delta Chat's released
   `deltachat-rpc-server`, as eeemail's `scripts/interop-pass.py` does.
4. Verify signed-only mail; desktop notifications.
