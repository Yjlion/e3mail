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
- The release packages have been built (see "Release packages" below), but
  none has been installed and run on its own platform.
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

**Not verified:** any of it on a real display, or by a native speaker.

## Release packages

The release workflow failed for both v0.1.0 and v0.1.1 and had never
succeeded. Three fixes (traps 17–19) made it green on all three platforms
through `workflow_dispatch` on `fix/release` (run 37527412735). That run
builds the packages but does not publish them, because publishing only
happens for tags.

**Verified:**
- The AppImage passes its checksum.
- It carries the xcb, Wayland and offscreen plugins, the Wayland shell
  integrations, and ICU.
- It boots offscreen in Arabic.
- The Windows portable zip carries RNP, Botan, ICU, SQLite, json-c,
  `qt.conf`, `portable.txt` and `qwindows.dll`.

**Not verified:**
- Nothing has been installed or started on Windows or macOS.
- The narrowed Windows installer glob was checked locally, not in a run.
- Nothing has been published yet. The project version is now 0.1.1, but the
  v0.1.0 and v0.1.1 tags point at commits without these fixes (and with
  version 0.1.0).

## Mail to self, and finding server settings (2026-10-07)

**Mail to self is encrypted.** `Policy` used to look up the own address as a
contact, find no key, and send cleartext (or refuse under strict mode). Worse,
a Bcc to self on mail to keyed peers made the whole message cleartext. The own
address now counts as keyed. `tst_engine::selfIsEncrypted` covers both cases
and fails without the fix.

**Setup asks six sources** ([ADR 0015](adr/0015-finding-server-settings.md)):
the provider's autoconfig file, ISPDB, Autodiscover, DNS SRV, ISPDB for the MX
host, and a guess. `e3mail-cli discover --addr` prints what it finds.

**Verified here:**
- `tst_autoconfig`: the parsers, and the order, the deadline, the redirect
  and abandoning a stale lookup against a fake network. Breaking the
  ordering failed 8 of its tests; dropping the stale-lookup check failed 1.
- Against the real network: gmail.com (ISPDB); fastmail.com, posteo.de and
  mailbox.org (provider); anthropic.com and microsoft.com (MX → ISPDB,
  needing review); kernel.org (guess). `QDnsLookup` returned gmail's and
  fastmail's real SRV records, but no real domain answered through SRV in
  the full lookup, because each was answered by a higher-ranked source.
- Setup screenshots, English and Arabic, with the source line and with
  server settings opened for review. They were grabbed through a temporary
  hook that typed an address; no script reproduces them.

**Not verified:** a real Autodiscover answer (fakes only), the Connect-during-
lookup wait in the running app, and any of it on Windows or macOS.

## Windows build time (2026-10-07)

Before: the Windows job took 25–40 minutes. vcpkg spent 23 min building Botan
and 8 min building ICU, on every release run (no cache) and on every CI run
after a version bump (the key hashed `vcpkg.json`, which carried the version).

Now `.github/actions/windows-deps` serves CI and the release from one cache,
built release-only (`x64-windows-release`), with a per-package vcpkg binary
cache behind it. CI runs on push only.

**Measured** (Windows job, wall clock):

| Case | Before | Now | Run |
|---|---|---|---|
| Nothing cached | 39–41 min | 26 min (Botan 15, ICU 4.7) | 37577790974 |
| Key changed, packages cached | 39–41 min | 5.7 min (dependencies 1.9) | 37583208476 |
| Full cache hit | 4 min, if the key survived | 4.1 min (dependencies 7 s) | 37583208476, re-run |
| Release, cache hit | 39–41 min | 4.7 min | 37584188287 |

The portable install still carries RNP, Botan, ICU, SQLite and json-c from the
release-only path. Getting here took three more rounds (traps 22–23, and
vcpkg.exe missing on a hit). A release on a tag restoring main's cache is
documented GitHub behaviour but has not been seen yet.

## Four gaps closed (2026-10-07)

**Large mailboxes.** IMAP's first sync fetches new mail first, then pages of
100 older messages, newest first, resumable through cursors in the config
(`imap_baseline`, `imap_high`, `imap_low`). Mail that was there before is never
announced, and a copy re-read under a new UIDVALIDITY keeps its first copy's
`preexisting` flag. A message deleted between listing and fetch is skipped.
The sidebar shows how many older messages are left.

**Desktop notifications.** Gathered per account for 2 s, quiet while the
window is in front, opening the message on click. A per-device setting in
`app.ini` (off / sender / sender and subject; default sender). Linux uses the
freedesktop service over D-Bus; Windows and macOS use the tray, so the app is
a `QApplication` there.

**Address book (P10).** Add, edit and remove contacts; organization, title,
phones, birthday and notes; vCard 2.1/3.0/4.0 import, 4.0 export. Ops
`contact.details` and `contact.remove` ([ADR 0013 amendment](adr/0013-op-merge-rules.md)).

**HTML replies.** Replies and forwards to HTML mail quote it as HTML;
`RichText::load` puts it in the editor, and the composer still re-emits
through its whitelist. Formatted drafts now reopen formatted.

**Verified here:**
- Tests: 12 suites. New: `tst_notifier`, `tst_vcard`, and in `tst_engine`
  `firstSyncPagesNewestFirst`, `firstSyncIsNotNewMail`, `addressBook`,
  `repliesQuoteHtmlAsHtml`; `tst_sync::addressBookTravels`. Each of the new
  rules (newest first, no notifying the first sync, the duplicate keeps its
  flag, the cross-key removal rule) was broken on purpose once and its test
  failed.
- `scripts/e2e.py` against `server/compose`: all passed with the new sync.
- 500 messages delivered to alice on the real Dovecot: five `sync` rounds of
  100, newest first (n499–n400, then down to n000), under a second each.
- A real D-Bus notification: the app under Xvfb on a private session bus,
  with a stand-in `org.freedesktop.Notifications` printing what it got. One
  message gave "Mira Dorn" / subject; a burst of three gave "3 new
  messages". Clicking (`ActionInvoked`) was tested only through the fake
  backend.
- CI (run 37631060889): Linux, macOS and Windows build and pass with Qt
  Widgets and the tray backend.
- Screenshots: an HTML reply (English, Arabic), the contact editor (English,
  Arabic, zoomed), Settings in German.

**Not verified:** a notification on Windows or macOS, or on a real desktop
session; clicking a real notification; any vCard from a real address book
(Google, Apple, Thunderbird exports) beyond the hand-written test cards.

## Known gaps

- **Signed-only mail** (`multipart/signed` without encryption) is shown, but
  its signature is not checked, so it reads as unsigned.
- **Inline PGP** (`-----BEGIN PGP MESSAGE-----` in a text body) is not
  decrypted.
- **Bcc and encryption**: Bcc recipients' key IDs are in the encrypted message,
  so recipients who inspect it can tell that Bcc recipients exist
  (ADR 0004).
- **POP3 is not paged.** Its first download is still one pass, now newest
  first; POP3 has no way to ask for part of a mailbox by order.
- **Address book**: one contact per address, so a person with two addresses
  is two contacts. No categories, postal addresses or photos.
- **Notifications** are shown only while e3mail runs; there is no background
  service.
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

The Wayland plugin's file name depends on the Qt version (trap 17).

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

**17. The Wayland platform plugin was renamed.** Since Qt 6.10 it is
`libqwayland.so`; Qt 6.8 (the release pin) has `libqwayland-generic.so` and
`libqwayland-egl.so`. Naming the new file built the AppImage here (Qt 6.11)
and failed on CI. The script now asks qmake which of them exist.

**18. Native Windows paths are not CMake paths.** `D:\a\...` passed in
`E3MAIL_BUNDLE_DLLS_FROM` was copied into `cmake_install.cmake`, where `\a`
is an invalid escape. Turn backslashes into slashes before using them in
install rules.

**19. Qt's deploy step needs an absolute install prefix**, because it writes
`qt.conf` there. In Git Bash use `$(pwd -W)`, which gives `D:/...`.

**20. The project version was in the cache key.** `vcpkg.json` carried
`version-semver`, and the Windows cache was keyed on its hash, so "Set the
version" cost a 40-minute rebuild. The version lives only in `CMakeLists.txt`.

**21. The vcpkg triplet is in every path.** `x64-windows-release` puts DLLs in
`vcpkg_installed/x64-windows-release/bin`. The host triplet is set to match.

**22. The vcpkg toolchain installs again at configure time.** On the first
cache hit (run 37580256256), the dependency step took 1 s, but `cmake` with
the vcpkg toolchain ran its manifest install, judged `vcpkg-make`'s ABI
changed, and rebuilt it and ICU: Configure took 6 min, and the result was
never saved. Why the hash differed between the two steps is not known.
`VCPKG_MANIFEST_INSTALL=OFF` makes configure only use what the dependency
step installed.

**23. Runner images carry different vcpkg checkouts.** Two runs started in the
same minute got `vcpkg-make@2026-07-09` and `vcpkg-make@2026-07-09#1`, so ICU's
ABI hash, and with it the per-package cache, depended on which image a job
landed on. Botan came back from that cache in 53 ms; ICU was rebuilt (5 min).
The action now checks out vcpkg at a pinned commit (`vcpkg-commit`) instead of
using `C:\vcpkg`. Bump it deliberately; that rebuilds everything once. It is
bootstrapped on every run, hit or not: the toolchain calls `vcpkg.exe` after
each link to copy DLLs, and without it a cache hit failed to link.

**24. Offscreen windows are active.** Under `QT_QPA_PLATFORM=offscreen` the
window counts as in front, so no notification is ever shown. A live check
needs Xvfb with `QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software` (Xvfb has no
GLX here), inside `dbus-run-session` with a notification service: this
machine's session bus has none.

**25. Qt logs to the journal** when stderr is not a terminal. Set
`QT_FORCE_STDERR_LOGGING=1` to see the app's log in a file.

**26. The fake mail server runs on the test's main thread.** Anything that
blocks that thread while a worker waits on the server deadlocks until the
socket's 120 s timeout. `Account::stop()` did, because IDLE waited for the
server to acknowledge DONE. Stopping now drops the connection; the engine
suite went from 123 s to 3 s.

**27. `QTextDocument::setHtml` is not the inverse of our emit.** `<p><br></p>`
comes back as a line holding a line break (two lines tall), headings come back
bold (so emit wrote `<h2><b>`), every paragraph gets 12 px margins, and a
`<pre>` ending in a newline gains an empty line. `RichText::load` undoes each.

**28. moc rejects a nested class with `Q_OBJECT`.** `Notifier::Backend` is
`NotifierBackend` with an alias.

**29. A new branch cannot read another unmerged branch's cache.** The first
Windows CI run on `known-gaps` rebuilt every dependency (29 min), because
`discovery-and-ci`'s cache is not on the default branch yet.

## Next

1. P8, slice 2: install Rust, build `src/p2p/` (Iroh, `iroh-blobs`) with
   Corrosion behind `DeviceTransport`, add the toolchain to CI on all three
   platforms, and wire `SyncSession` into `Account`.
2. P8, slice 3: QR pairing with the encrypted snapshot; the mailbox fallback.
3. Publish v0.1.1: move its tag to a commit that has the release fixes
   (nothing was published under it), then install each package on its own
   platform.
4. P9 SecureJoin, then an interop pass against Delta Chat's released
   `deltachat-rpc-server`, as eeemail's `scripts/interop-pass.py` does.
5. Verify signed-only mail.
6. Try notifications on a real Windows and macOS desktop, and import real
   vCard exports (Google, Apple, Thunderbird).
