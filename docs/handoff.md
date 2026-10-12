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

**Not built yet** (at the time): the Rust crate, pairing, the mailbox
fallback, any UI, and the wiring into `Account`. All of these are built as of
2026-10-11; see the sections below.

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

## Newsletters render (2026-10-07)

A forwarded Proton newsletter showed its text in a column a few words wide,
with its logos and banner listed as attachments. Two causes, both fixed:

- **Nested layout tables.** Qt's rich text sizes a nested table by its
  narrowest content. The sanitizer now turns layout tables (`role=
  "presentation"`, or any table holding another table) into blocks; tables of
  data stay tables. Stored mail is sanitized when shown, so old mail benefits.
- **Inline images** (`cid:`) were replaced by alt text. They now show, through
  an `image://cid/` provider fed from the stored parts ([ADR 0009
  amendment](adr/0009-two-barriers-for-message-content.md)), and are no longer
  listed as attachments.

**Verified:** the message rendered through the real app, from Dovecot on the
test server, before and after (top and bottom of the message looked at);
`tst_mime` covers the table rule and the `cid:` rule.
**Not verified:** other senders' newsletters; a row of icons in layout cells
now stacks vertically, which is readable but not the sender's layout.

## Android (2026-10-07)

e3mail builds for Android as the same app ([ADR 0016](adr/0016-android.md)).
Below 720 px the window shows one pane at a time: a drawer for the sidebar, an
opened message covering the list, a title bar with the menu or Back. Contacts
does the same for its list and detail. Desktop windows may now shrink to
360 px and get the same layout. Notifications go through Android's own while
the app runs, and it syncs on returning to the front; there is no background
service. Passwords go to the Android Keystore through QtKeychain.

**Verified here:**
- The desktop: all 14 suites, including the new `app_smoke_narrow` and
  `app_smoke_narrow_rtl`. Phone-size screenshots (inbox, drawer, message,
  composer, contacts, settings, first run; Arabic for inbox, message and
  drawer; German settings at full length) were looked at. They found real
  overflow in Settings, the dialogs and the message header, now fixed.
- `scripts/android-deps.sh x86_64` built every dependency (about 40 minutes
  here, ICU and Botan most of it), and `cmake --preset android-x64` built a
  signed 43 MB APK with warnings as errors.
- On an API 34 x86_64 emulator: it installs and starts, asks for the
  notification permission, and shows setup with the first-run dialog. The
  accessibility tree (`uiautomator dump`) lists the expected controls, and
  logcat has no QML errors. "I understand" dismissed the dialog.

**Not verified:**
- **Rendering.** The emulator here (3 cores, under 6 GB, no GPU) draws Qt
  with torn and missing areas under both `swiftshader_indirect` and `guest`.
  With `QT_QUICK_BACKEND=software` (passed as the `extraenvvars` intent extra)
  most of setup drew correctly, but the radio buttons, the Connect button and
  the field borders were still damaged. CI's emulator (run 37721374406, a
  different host, also SwiftShader) shows the same diagonal tears and the
  first-run dialog bleeding through, so it is less likely to be this machine
  alone. Whether it is SwiftShader or ours is not known. Look at it on a real
  phone first.
- **Typing.** `adb shell input text` arrived scrambled ("@ensomeanthropic.como"),
  probably the IME's composition against Qt over a slow emulator; not tried by
  hand. The keyboard covered the Connect button: `adjustResize` did not shrink
  the window.
- **Any mail.** The setup form has no way to accept the test server's
  self-signed certificate (only `e3mail-cli --insecure` has), so no account
  was added, and nothing was received, sent, notified, or stored in the
  Keystore. Server discovery on Android, and whether `QDnsLookup` works there,
  are untested for the same reason (typing failed).
- arm64-v8a was not built here. CI (run 37721374406) built both APKs, booted
  the x86_64 one in an API 34 emulator with no QML errors in logcat, and
  passed on Linux, macOS and Windows. Nothing has run on an arm64 device.

## Exports, mail to self, All mail, APKs in releases (2026-10-08)

- **Mail to self is in the Inbox too** ([ADR 0007 amendment](adr/0007-tags-not-folders.md)).
  The server's copy of mail we send to ourselves was, and still is, dropped
  as a duplicate of the Sent row (one Message-ID, one row). The Inbox clause
  now also takes sent mail with our address in To, Cc or Bcc. It is derived:
  no migration, no op. It shows read.
- **All mail** has its own place in the sidebar. `E` archives the selected
  message (and moves it back to the Inbox when viewing Archive). The row
  menu says "Move to Inbox" in Archive.
- **Source view.** `mime::Source` gives the MIME outline, shortens runs of
  base64 (attachments, armour, Autocrypt keydata) to one marker, and builds
  a decrypted `.eml`. The dialog highlights header names, boundaries and
  armour (`SourceHighlighter`, a `QSyntaxHighlighter`). It has Original and
  Decrypted views, Copy, Save, and the structure.
- **Message export.** "Save as .eml" writes the original as received or
  sent, so encrypted mail stays encrypted. "Save decrypted as .eml" warns
  first, then writes the outer headers with the inner part's protected
  headers winning. The CLI has `show <id> --raw [--decrypted]`.
- **Contacts.** Export one contact as a vCard, and the address book as vCard
  or CSV: RFC 4180, UTF-8 with a BOM, and an apostrophe before a cell that
  would run as a formula, except phone numbers. The CLI has
  `contacts-export [--csv]`. `mail::Export::addressBook` replaces the filter
  that was duplicated between the app and the CLI.
- **Files go through `FileIo`**: `QSaveFile` for local paths, `QFile` on the
  URL for Android's `content://`. This covers attachments, contact
  import/export and messages.
- **Releases carry both APKs** with checksums, signed with a key from the
  repository's secrets, or a throwaway one with a warning
  ([ADR 0016 amendment](adr/0016-android.md) has the one-time setup).
- `todo.md` lists the next large pieces, none started: Iroh sync, encrypted
  cloud backups, local AI through llama.cpp, and Mox/Maddy administration.

**Verified here:**
- `ctest --preset dev`: 14 suites pass. New tests:
  - `tst_engine::sentToSelfInInbox`: To, Cc and Bcc to self, mail to someone
    else, archive and trash, All mail. Removing the new clause failed it, and
    `selfIsEncrypted`, whose Inbox count changed from 0 to 1.
  - `tst_engine::selfIsEncrypted`: a real encrypted note exported as is
    (still `multipart/encrypted`) and decrypted (subject and body readable).
  - `tst_mime::sourceOutline`, `sourceShorten`, `decryptedEml`.
  - `tst_vcard::csv` (quoting, BOM, the formula rule, phones left alone) and
    `singleCard`.
- Screenshots looked at:
  - the source dialog in English, German, Hebrew and Arabic (`source`,
    `source-ar`), and on a phone (`phone-source`);
  - the Decrypted view with its structure, and the contact detail with
    Export vCard on a phone in English and Arabic, through temporary edits
    that are not in the tree;
  - the sidebar with All mail.
- The translations are complete in all ten languages (machine-made, like
  the rest).
- The release workflow through `workflow_dispatch` (run 37828323535), with
  the signing secrets set. All five packages built, after one re-run: the
  first Windows attempt got HTTP 500 downloading `vcpkg.exe`. Both APKs are
  signed by `CN=e3mail`, the repository's key, with certificate SHA-256
  `18:DD:0C:5D:…:72:02:A4`, and their checksums match. The Android jobs took
  4 minutes, on the dependency cache CI had built. Nothing was published:
  there was no tag.

**Not verified:**
- `scripts/e2e.py`: Docker was not reachable from this session (permission
  denied on the socket), so mail to self was not tried against
  `server/compose`. The fake server covers it.
- Saving to a `content://` URI on Android, for any of the exports.
- Saving any file from a real file dialog; offscreen there is none.
- Opening a decrypted `.eml` in Thunderbird or another client.
- A CSV opened in Excel, LibreOffice or Google Contacts.

## Typing formats, several signatures, signed-only mail (2026-10-10)

- **Formatting applies to what is typed next.** Bold, italic and the rest
  did nothing without a selection: the composer only changed selected text.
  QML does not expose the editor's own cursor, so `TextFormatter` keeps a
  format chosen with nothing selected as *pending* and lays it on the next
  text typed at that spot; what follows inherits it from the character
  before. Enter alone carries it to the new line; moving the cursor away
  drops it. The toolbar buttons show the format at the cursor. The toolbar
  and editor are one component, `RichEditor.qml`, used by the composer and
  the signature editor. `toggleFormat`, `setBlock` and `setLink` moved out
  of `Composer`.
- **Several signatures, formatted, one (or none) the default**
  ([ADR 0013 amendment](adr/0013-op-merge-rules.md)). Each is a synced
  setting `signature:<id>`; `mail::Signatures` reads and writes them. The old
  plain signature reads as the one with id `legacy` until edited. Settings
  lists them with a default radio, Edit, Delete and "No signature in new
  mail"; `SignatureDialog.qml` edits one through `RichEditor`, and what is
  saved is `RichText::emit`'s output, the same whitelist as sent mail.
- **The composer's signature button** opens a menu of signatures and "No
  signature". It swaps the signature the composer put in for the chosen one
  while that signature's text is unchanged (`SignatureRange`); once edited,
  it is left alone and the new one goes in at the cursor.
- **Signed-only mail is verified** (it was Next #5). `multipart/signed` with
  `application/pgp-signature`: the detached signature is checked over the
  first part as sent, in CRLF, and checked again after the same message's
  Autocrypt header taught the key (trap 4). `Pgp::verifyDetached` and
  `signDetached` (the latter for tests).

**Verified here:**
- `ctest --preset dev`: 15 tests pass. New:
  - `tst_formatting`: a real `TextArea` offscreen, typed into with key
    events. Bold on, type, bold off, type: only the middle is bold; two
    formats at once; pressing twice cancels; moving away drops it; Enter
    keeps it; a selection still toggles; the state follows the cursor.
    Turning off the merge of the pending format failed four of its six.
  - `tst_richtext`: `RichText::insert` (formatting kept, the script
    dropped) and `swapsAnUntouchedSignature` (text typed above and right
    after is not part of it, an edited one is never taken). Dropping
    `setKeepPositionOnInsert` failed it.
  - `tst_engine::signatures` (legacy, default, removal, nine `config.set`
    ops) and `signedOnlyIsVerified` (a good signature, a tampered body, the
    key arriving in the same message, bare LF from the server). Removing the
    CRLF rule, or the second verification, failed it.
  - `tst_sync::signaturesMergeOneByOne`: two devices edit different
    signatures at once and both edits survive; a removal travels with the
    default. Dropping the prefix from `Preferences::isSynced` failed it.
  - `tst_crypto::gnupgDetachedSignatures`: GnuPG's detached signature
    verifies here and a changed text does not; ours verifies in GnuPG.
- Screenshots looked at: the composer with the toolbar and a formatted
  signature, a reply to HTML mail with it, Settings with the list (English,
  Arabic, phone), the signature editor (English, Arabic, phone). New shots:
  `composer-formatting`, `signature`, `signature-ar`, `settings-ar`,
  `phone-signature` (pages `formatting` and `signature`).
- Translations complete in all ten languages (machine-made).

**Not verified:**
- Typing on a real display, and above all through an input method: Android's
  keyboard and CJK composition commit text differently from key events, and
  whether the pending format catches that commit is untested.
- The signature menu in the composer: it is a popup, so no screenshot shows
  it, and the swap was tested on `SignatureRange`, not through the view.
- Signed-only mail from a real client (Thunderbird, Delta Chat, K-9). The
  fixtures are made here and by GnuPG. `scripts/e2e.py` against
  `server/compose` passes with this work (after adding the user to the
  `docker` group), but it sends no signed-only mail.
- Signatures syncing over anything but the loopback transport.

## Trap 38 fixed, inline PGP, GnuPG through Dovecot (2026-10-11)

- **Stopping no longer waits for the server.** Trap 38 was real: stopping an
  account blocked the main thread in `wait()` while the worker sat in a
  socket wait that only the 120 s timeout ended (the retention pass, or the
  IDLE handshake). In the tests the fake server lives on that blocked
  thread. `SyncSocket` waits now poll `ServerSettings::abort`, which the
  worker sets to its stop flag. The fake server forgets dead idlers and
  announces mail that came between SELECT and IDLE.
- **Inline PGP** is decrypted and verified: the first armoured block in the
  text part (a PGP message or a cleartext-signed text) is replaced by what
  it holds, decoded in the part's charset, with the text around it kept.
  An HTML alternative showing the same armour is dropped. Verification is
  tried again after the message's own Autocrypt header taught the key.
  "Save decrypted" writes the text.
- `e3mail-cli key` prints the account's public key, armoured.

**Verified here:**
- `tst_engine::stopDoesNotWaitForTheServer`: a stalled fake server, then
  `stop()` returns in about 1 s. Without the fix it took 119.5 s, trap 38
  exactly. The engine suite ran five times in 4.0 to 4.5 s.
- `tst_engine::inlinePgp`: encrypted and signed in Latin-1 with text around
  it, cleartext-signed, the same tampered, a stranger's key, and an HTML
  alternative; the decrypted export. Turning off the inline step, or its
  second try, failed it.
- `tst_crypto::gnupgClearsigned`: GnuPG's `--clearsign`, read with and
  without the signer's key, dash-escaping undone, a tampered text unsigned.
- `scripts/e2e.py` against `server/compose`, all passed, with a new step 5:
  a GnuPG sender's PGP/MIME signed, cleartext-signed and inline encrypted
  mail, delivered by Dovecot's LDA, each arrives signed (and encrypted),
  not verified, and without armour.

**Not verified:** inline PGP from a real client (Enigmail-era Thunderbird,
Mailvelope); a message holding more than one armoured block (only the first
is read); PGP/MIME inside an inline message.

## Self-signed servers: certificate pinning (2026-10-11)

[ADR 0017](adr/0017-certificate-pinning.md). When TLS refuses a server's
certificate, setup shows why, its SHA-256 fingerprint, subject, issuer and
expiry, and "Trust this certificate". Trusting pins that one certificate for
that server (`in_cert_sha256`, `smtp_cert_sha256`, never synced); any other
certificate from it is refused, even one TLS would trust. SMTP on the same
host with the same certificate is pinned along. A running account whose
certificate is refused goes to status `cert`; Settings shows the new
fingerprint with "Trust this certificate", and the pinned ones with "Forget".
`e3mail-cli cert` prints a server's certificate; `add --in-cert/--smtp-cert`
pin from the command line. `--insecure` stays for tests.

**Verified here:**
- `tst_net::certificatePinning` (implicit TLS and STARTTLS against a
  `QSslServer` with the committed `tests/data/tls-cert.pem`): refused with the
  right fingerprint, subject and reasons; accepted when pinned; refused when
  another is pinned. `pinRefusesATrustedCertificate`: with the certificate
  added as a CA, a different pin still refuses it. Not ignoring the pinned
  certificate's errors failed the first; dropping the check after the
  handshake failed the second.
- `scripts/e2e.py`: `e3mail-cli cert` reads the test server's self-signed
  certificate on all five ports; carol now logs in over STARTTLS with the
  pin instead of `--insecure`; a wrong pin is refused. All passed.
- The real setup form against `server/compose` (a temporary hook filled in
  carol and pressed Connect, then Trust; not in the tree): the card showed
  the server's fingerprint and Qt's reason, in English and Arabic, and
  trusting created the account with both pins stored.

**Not verified:** the Settings card for a changed certificate (no test
server rotated its certificate); Android reaching `server/compose` through
setup, which this was for (Next #7); a certificate chain where the first
certificate is not the server's own.

## P8 slice 2: devices over Iroh (2026-10-11)

The transport is in ([ADR 0010 amendment](adr/0010-multi-client-over-iroh.md)).
`src/p2p/` is a Rust crate (`e3p2p`, iroh 1.3, no iroh-blobs) with a
non-blocking C ABI (`e3p2p.h`), built by cargo through Corrosion
(`cmake/E3mailP2p.cmake`, `E3MAIL_P2P`, on by default).
`sync::IrohTransport` implements `DeviceTransport` over it: one QUIC
connection per pair of devices, framed messages, a hello naming the device,
strangers refused by the crate before a byte is read. `Account` starts it
with the worker when a device with an endpoint is paired, pushes after every
change (200 ms debounce), and dials paired devices every minute. Migration 4
gives `devices` an endpoint id, an address and the peer's last version map.
Each device's Iroh key (`p2p_secret`) and relay setting (`p2p_relay`) stay
on that device.

**Verified here:**
- `cargo test` (the ctest `p2p_cargo`): framing; two endpoints on this
  machine with the relay off exchange frames both ways, one of 3 MiB; a
  stranger is closed without its frame being delivered.
- `tst_p2p`, two devices of one account over real endpoints, relay off: a
  message B never fetched arrives from A as a raw blob, decrypts, and A's
  read state follows; B archives and A follows live; an unpaired endpoint
  gets nowhere; after B's endpoint stops and starts again it is dialed back
  and changes flow. Five runs, 2.7 s each. Letting every endpoint in (in
  the crate) failed the stranger test.
- `tst_sync` unchanged, now using the shared `tests/DeviceClone`.
- `cmake --preset android-x64` builds a signed APK with the crate inside
  (50 MB, was 43), once cargo links through the NDK's
  `x86_64-linux-android28-clang` (trap 43).

**Not verified:**
- Any relay, or n0's address lookup: every test runs with the relay off on
  one machine. Two machines on different networks have never connected.
- Installing any of it on its own platform. CI (run 38189964407, branch
  `compose-formats-signatures`) builds and tests everything with the crate
  on Linux, macOS and Windows, builds both APKs and boots the x86_64 one in
  an emulator (trap 48). The release workflow has not run with Rust.
- A large first transfer: blobs travel one frame each (up to 64 MiB), with
  no flow control beyond QUIC's; a mailbox of several gigabytes has not been
  tried.

## P8 slice 3, first half: pairing (2026-10-11)

A new device joins an account with a code ([ADR 0010 amendment](adr/0010-multi-client-over-iroh.md)).
Settings, Devices, "Pair a new device…" shows a QR code and the same code as
text; on the new device, setup offers "Copy an account from another
device…", where the code is pasted. The existing device asks whether to let
"Laptop" in; then the new device pulls the database and every blob, checked
by SHA-256, plus the passwords, and becomes a device of its own (id, Iroh
key, name). A `device.pair` op tells the other devices, and Remove sends
`device.unpair` and drops the device at once (ADR 0013 amendment). Settings
lists the devices, online or last seen, the device's own name, and the
relay: n0's public relays, one the person runs, or none.

`engine/Pairing` holds both sides. `e3mail-cli` gains `devices`,
`pair-offer [--yes]`, `pair-join <code>` and `run [seconds]`, and
`scripts/p2p-e2e.py` uses them.

**Verified here:**
- `tst_p2p` (9 tests, relay off, loopback only): a full pairing (the new
  device has the mail, decrypts it, has its own id, key and name, the
  password, and server acks for what the copy holds; A records it and B
  learns of it through `device.pair`; then A's read state reaches it); the
  code refused a second time; Deny refused; a wrong token never asked
  about; a blob changed on disk refused by its hash; a removed device
  dropped and refused. Thirty runs in a row without a failure, after
  trap 45.
- `scripts/p2p-e2e.py` against `server/compose`, three runs, all passed:
  two `e3mail-cli` processes pair with a code; the server keeps alice's new
  message until the second device has it; it reaches B; A's accept and
  archive reach B; then the server copy is deleted.
- `scripts/e2e.py` still passes.
- Screenshots: Devices (English, Arabic, phone), the pairing dialog with its
  QR code (English, Arabic, phone), the join dialog.

**Not verified:**
- Scanning the QR code: there is no camera scanning; the code is pasted. No
  phone has read the QR code shown on screen.
- Pairing over n0's relay, or between two machines.
- A large mailbox: the copy is pulled in 1 MiB pieces, eight at a time,
  measured only on small fixtures.
- Removing a device while it is offline: it is refused when it comes back,
  and the other devices learn of the removal, but what the removed device
  shows then has not been looked at.

## P8 slice 3, second half: the mailbox fallback (2026-10-11)

Devices that are never online together sync through the mailbox
([ADR 0010 amendment](adr/0010-multi-client-over-iroh.md)). Five minutes
after a change, if a paired device is away and lacks this device's newest
ops, `sync::MailboxFallback` queues a mail to the account's own address:
`X-E3-Sync`, encrypted to the account's key and signed with it, holding the
sender's own ops (up to 500) and its whole version map. Ingest hides it and
applies its ops only if the account's own key signed it and the sender is a
paired device. Phase 8 is done.

Two fixes came out of this work:
- **A replaced connection lost what was sent on it.** When both devices
  dialed at once and the newer connection won, `SyncSession` was not told,
  so ops sent on the closing connection counted as delivered and were never
  sent again. `scripts/p2p-e2e.py` failed about one run in five because of
  it. The switch now looks like a reconnect.
- **Hidden sync mail is no longer recorded as `msg.add`**: every device
  gets it from the server.

**Verified here:**
- `tst_sync::mailboxFallback`: two devices that never connect; A's read
  state reaches B by mail, the mail is hidden, a second call sends nothing,
  B's answer tells A what it holds, nobody mails again, and the server lets
  go of everything but the last sync mail.
  `mailboxFallbackInBatches`: 600 ops go as 500, an answer, then the rest
  (fails without the answer to a full mail). `forgedSyncMailIsIgnored`: a
  sync mail Bob signed, claiming to be A, trashes nothing.
- `tst_p2p::simultaneousDials`: five rounds of both dialing at once; each
  change arrives. Without the reconnect fix it failed every time.
- `scripts/p2p-e2e.py`: ten runs in a row passed after the fix.
  `scripts/e2e.py` passes.

**Not verified:**
- The fallback against `server/compose`: the live script runs both devices
  at once, so it never needs the mailbox. It was tested against the fake
  server only.
- The five-minute timer in the running app: tests call `mailFallbackNow()`.
- The last sync mail stays on the server until the devices next exchange
  anything; how many pile up over weeks of one device being away has not
  been measured.

## Known gaps

- **Signed-only mail** is checked only for PGP/MIME (`multipart/signed`);
  S/MIME signatures read as unsigned.
- **Inline PGP**: only the first armoured block of the text part is read.
- **Bcc and encryption**: Bcc recipients' key IDs are in the encrypted message,
  so recipients who inspect it can tell that Bcc recipients exist
  (ADR 0004).
- **POP3 is not paged.** Its first download is still one pass, now newest
  first; POP3 has no way to ask for part of a mailbox by order.
- **Address book**: one contact per address, so a person with two addresses
  is two contacts. No categories, postal addresses or photos.
- **Notifications** are shown only while e3mail runs; there is no background
  service.
- **Android** has no background sync: mail arrives only while e3mail is open.
  Its rendering, typing and mail flow are unverified (see "Android" above).
  CI packages are debug-signed; release packages use the key in the
  repository's secrets once it is set (ADR 0016 amendment).
- **Sync**: devices have only been tried on one machine, with the relay off;
  the pairing code is pasted, not scanned. A message another device sent while it was still pending in that
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

**30. `Q_OS_LINUX` is defined on Android.** Code that means the desktop
Linux (D-Bus, the freedesktop notifier) must test `Q_OS_ANDROID` first.
CMake's `CMAKE_SYSTEM_NAME` is `Android`, so CMake branches are not fooled.

**31. NDK r26b cannot build Botan 3**: its libc++ lacks an `operator<=>`
Botan needs (`IPv6Address`). r27c can, and Qt 6.8.3 for Android works with it.

**32. An interrupted aqt install looks complete.** The first toolchain run hit
the disk quota (aqt downloads into `/tmp`; set `TMPDIR`) after the desktop
Qt's base but before its ICU. The rerun saw the directory and skipped it, and
`qtpaths` then failed on `libicui18n.so.73`. The script now trusts a marker
written after a full install.

**33. `pgrep -f`/`pkill -f` match the shell running them** when the pattern
is in its own command line. Killing `qemu-system` that way killed the command
itself.

**34. The emulator needs memory.** At `-memory 2048` with a Gradle daemon
left over from the build, the kernel's OOM killer ended the emulator. Kill
the daemon after building and use `-memory 1536`; Android then restarts the
app in the background now and then. `settings put global hide_error_dialogs 1`
stops the "System UI isn't responding" dialogs covering the app.

**35. A `ListView`'s `leftMargin` changed after layout is not re-applied**,
so a binding on `Theme.narrow` left the desktop's message cards at x = 0. The
margin lives inside the delegate now.

**36. A layout item that does not fill keeps its implicit width.** A
`CheckBox` or `Label` with long text in a `ColumnLayout` pushed the whole
column wider than a phone, clipping everything. They fill and wrap on narrow
windows now.

**37. A short right-to-left label lost its CheckBox padding.** In the
source dialog's `Flow`, in Hebrew and Arabic, the Basic `CheckBox`
"Structure" (מבנה, البنية) reported an implicit width without its
indicator's padding. The next button covered the label, which looked as if
it had not been translated. Its neighbour with a longer label was fine, and
the same word in a minimal QML file was fine. The cause is not known. The
dialog's checkboxes measure themselves (`TextMetrics`). Do not use
`contentWidth` instead: it follows the elided width, and both checkboxes
shrank.

**38. `firstSyncIsNotNewMail` sometimes takes 122 s.** It hit what looks
like trap 26's 120 s socket timeout twice in one session, once on a tree
without this work's changes. It took 2 to 3 s in eight runs after that. It
passes either way. Not investigated.

**39. The editor reports text before it moves the cursor.** Typing into a
`TextArea` emits the document's `contentsChange` while `cursorPosition` still
holds the old position. `TextFormatter` does not rely on the order: it drops
a pending format on a cursor move only in a queued call, after any insertion
has had its turn.

**40. `QTest::keyClicks` takes only a `QWidget`.** For a `QQuickView`, send
`QTest::keyClick` per character.

**41. Transactions do not nest.** `Transaction` is a bare `BEGIN IMMEDIATE`.
Code holding one calls `Preferences::write`, not `set`, to change a setting.

**42. Trap 38 again.** One full `ctest` run here took 135 s instead of 16;
the next took 16 s. Fixed on 2026-10-11 (see "Trap 38 fixed").

**43. Cargo links Android dependencies with a bare clang.** Corrosion sets
the NDK's `clang` as cargo's linker and passes `--target` and the sysroot
only for the final crate. iroh-relay also builds a shared library, which
then failed with `unable to find library -llog`. `cmake/E3mailP2p.cmake`
sets the NDK's per-target wrapper (`<triple><api>-clang`) instead.

**44. A Rust target directory under `src/p2p/`** appears when cargo is run
there by hand (2.6 GB after one `cargo test`). It is in `.gitignore`; CMake
builds in `build/`.

**45. Rust may call the waker after `e3p2p_stop` returned.** Shutting the
runtime down in the background lets a connection task finish later and call
the waker on an `IrohTransport` already freed; it crashed one run in nine.
The waker now sits behind a lock that `e3p2p_stop` clears first. Separately,
a pairing failure must not delete the transport from inside its own poll
(heap corruption): it is `deleteLater`.

**46. Screenshots must not picture this machine.** The join dialog's
default device name is the host name, and a pairing code holds the
endpoint's addresses. `scripts/screenshots.sh` sets `E3MAIL_DEVICE_NAME`,
and the fixtures' endpoint is loopback-only (`p2p_loopback`), as in the
tests.

**47. A Qt Test that hangs survives `timeout`.** On SIGTERM QtTest tries to
print a stack trace with gdb and waits. Use `QTEST_DISABLE_STACK_DUMP=1` and
`timeout -s KILL` when running a suite that might hang.

**48. The first CI run with Rust failed twice.** macOS left CoreFoundation,
Security and SystemConfiguration symbols unresolved: iroh's dependencies use
them, and Corrosion adds only the standard library's. On Windows the build
and every C++ test passed, but `cargo test` ran in Git Bash, where Git's
`link` comes before MSVC's. `cmake/E3mailP2p.cmake` links the frameworks and
names MSVC's linker for that test.

**49. The first release run with Rust failed on Windows the same way**
(run 38259540456, tag v0.2.0, so v0.2.0 was never published). The release
workflow builds in Git Bash, where CI builds in PowerShell, and cargo linked
the build scripts of `proc-macro2` and `quote` with Git's `link`. Corrosion
leaves the linker to rustc with MSVC and ignores `corrosion_set_linker`, so
`cmake/E3mailP2p.cmake` now sets `CARGO_TARGET_<triple>_LINKER` to
`CMAKE_LINKER` for the crate build and the test alike (from
`Rust_CARGO_TARGET_CACHED`: plain `Rust_CARGO_TARGET` is empty unless a preset
sets it, as Android's do, and the first try named `CARGO_TARGET__LINKER`). The
target is the host, so the one variable covers build scripts too.

## Next

1. Connect two machines on different networks through n0's relay, and run
   the release workflow with Rust.
2. Scan the pairing QR code with a phone's camera (Qt Multimedia and a QR
   decoder in the Android build); the code is pasted for now.
3. Install each published package on its own platform. Pre-releases
   v0.1.1 to v0.1.5 are on GitHub (v0.1.5 with both APKs); v0.1.0 is still
   marked Latest.
4. P9 SecureJoin, then an interop pass against Delta Chat's released
   `deltachat-rpc-server`, as eeemail's `scripts/interop-pass.py` does.
5. Try notifications on a real Windows and macOS desktop, and import real
   vCard exports (Google, Apple, Thunderbird).
6. Install v0.1.5's arm64 APK on a phone and try each export there
   (Android's file picker returns `content://`).
7. Android on a real phone: rendering, typing (including a bold word typed
   through the keyboard, see "Typing formats" above), the keyboard covering setup's
   Connect button, then a real account (receive, send, a notification, the
   Keystore). Setup can now trust `server/compose`'s self-signed certificate
   (ADR 0017); from the emulator the host is `10.0.2.2`.
