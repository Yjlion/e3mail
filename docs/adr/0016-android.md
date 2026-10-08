# 0016 — Android: the same app, one pane at a time, only while it runs

**Status:** Accepted — 2026-10-07

e3mail runs on Android as the same program: the engine, the controllers and
the QML are the desktop's, built with Qt for Android and packaged by
`androiddeployqt`. There is no second interface and no Java mail code.

**The interface adapts by width.** Below 720 pixels (`Theme.narrow`) the
window shows one pane at a time:
- the sidebar becomes a drawer, from the start edge;
- the message list fills the window, and an opened message covers it;
- a title bar offers the menu, or a way back.

Android's Back key and the bar's arrow take the same step back. Wider windows,
tablets included, keep the three panes, and the desktop does not change.

**Sync and notifications only while e3mail runs.** There is no background
service. Android ends idle connections when an app leaves the screen, so
e3mail syncs when it comes back to the front. While it runs but is not in
front, new mail is announced through Android's notifications (`E3Notify`),
under the same per-device setting as on the desktop. A background service
(WorkManager, or a foreground service holding IMAP IDLE) is a separate
decision, about battery use and about what may run without the person
watching.

**Passwords go in the Android Keystore**, through QtKeychain built for
Android. ADR 0012 applies unchanged, fallback included.

**No backup.** `allowBackup="false"` and data extraction rules that exclude
everything. Android's backup would copy the database, and with it the secret
key, to the cloud without our encryption. Moving to a new phone is pairing's
job (P8).

**TLS** is Qt's OpenSSL backend, which loads `libssl_3.so` at run time. The
package carries KDAB's prebuilt OpenSSL 3 builds (`android_openssl`), the
copies Qt's own documentation points to.

**The native dependencies are static.** vcpkg builds Botan, json-c, ICU,
SQLite, zlib and bzip2 for each ABI (release only), and RNP and QtKeychain are
built against them (`scripts/android-deps.sh`). ICU's data makes the package
about 30 MB larger; the system's ICU is not part of the NDK's stable API.

**Minimum API 28** (Android 9). It is the lowest level vcpkg's Android
triplets and Qt 6.8 are both comfortable with. The target is API 34.

**Consequences.**
- Mail arrives only while e3mail is open. The known gaps list says so.
- One codebase: every QML change is checked at phone width too
  (`app_smoke_narrow`, and the phone screenshots).
- CI builds both ABIs and boots the x86_64 package in an emulator. Packages
  are debug-signed; signing for release is not set up.
