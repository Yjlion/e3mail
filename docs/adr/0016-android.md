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

## Amendment — 2026-10-08: release packages and their key

The release workflow publishes both APKs (`e3mail-android-arm64-v8a.apk` for
phones, `e3mail-android-x86_64.apk` for emulators and Chromebooks), each with
a SHA-256 checksum, beside the desktop packages.

They are signed with one key kept in the repository's secrets, because Android
installs an update only over a package signed with the same key; a key made
per run would make every update an uninstall, and an uninstall deletes the
mailbox. CI keeps its throwaway key: its packages are not published.

Setting the key up, once:

```sh
keytool -genkeypair -keystore e3mail-release.keystore -alias e3mail \
    -keyalg RSA -keysize 4096 -validity 10000 -dname "CN=e3mail"
base64 -w0 e3mail-release.keystore   # the value of ANDROID_KEYSTORE_BASE64
```

Secrets: `ANDROID_KEYSTORE_BASE64`, `ANDROID_KEYSTORE_PASS`,
`ANDROID_KEY_ALIAS` (`e3mail` above) and `ANDROID_KEY_PASS` (may be omitted
when it equals the store password). Keep a copy of the keystore outside
GitHub: losing it means no release can update an installed e3mail again.
Without the secrets the workflow still builds, signs with a throwaway key and
warns.

## Amendment — 2026-10-12: Google Play

e3mail is also published on Google Play, beside the APKs on GitHub.

**Qt 6.10, API 36, 16 KB pages.** Play takes new apps and updates only when
they target API 36 (since 2026-08-31) and when every native library is
aligned for 16 KB pages. The free Qt 6.8 stops at 6.8.3, whose Android
libraries are aligned to 4 KB; Qt 6.10 has both. The whole project moves to
Qt 6.10.3, desktop included, so there is one Qt to test. The minimum stays
API 28.

**Predictive back is off** (`enableOnBackInvokedCallback="false"`).
Targeting API 36 turns it on, and Android then stops sending the Back key,
which the narrow layout steps back with.

**A bundle, arm64-v8a only.** Play takes Android App Bundles, not APKs. The
release workflow builds one from the arm64-v8a build, which covers phones;
x86_64 (emulators, Chromebooks) stays a GitHub download. A second bundle
for x86_64 would need a version code of its own, which the scheme in
`src/app/CMakeLists.txt` does not leave room for yet.

**Google holds the app signing key** (Play App Signing). The workflow signs
the bundle with an upload key kept in the repository's secrets
(`PLAY_UPLOAD_KEYSTORE_BASE64`, `PLAY_UPLOAD_KEYSTORE_PASS`,
`PLAY_UPLOAD_KEY_ALIAS`, `PLAY_UPLOAD_KEY_PASS`); Google re-signs it. A lost
upload key can be replaced through Play's support, unlike the GitHub release
key. The consequence: a package from Play and one from GitHub carry
different signatures, so neither installs over the other. Moving between
them is an uninstall, which deletes the mailbox; pairing (P8) is the way to
move.

**Every tagged release goes to the internal testing track, as a draft**
(`PLAY_SERVICE_ACCOUNT_JSON`, a service account with release rights on this
app only). A person releases it to testers and promotes it in Play Console;
nothing reaches users without that step. The tag must equal the project
version, because Play refuses a version code it has seen (v0.2.0 was tagged
at 0.1.5). Without the secrets the workflow builds, warns, and uploads
nothing.

Setting it up, once:

1. Play Console: create the app (`org.e3mail.e3mail`), choose to let Google
   create and keep the app signing key, and fill in the store listing,
   privacy policy, Data safety, content rating and target audience.
2. The upload key:

   ```sh
   keytool -genkeypair -keystore e3mail-upload.keystore -alias upload \
       -keyalg RSA -keysize 4096 -validity 10000 -dname "CN=e3mail upload"
   base64 -w0 e3mail-upload.keystore   # PLAY_UPLOAD_KEYSTORE_BASE64
   ```

   Secrets: `PLAY_UPLOAD_KEYSTORE_BASE64`, `PLAY_UPLOAD_KEYSTORE_PASS`,
   `PLAY_UPLOAD_KEY_ALIAS` (`upload`), and `PLAY_UPLOAD_KEY_PASS` (may be
   omitted when it equals the store password).
3. The first bundle goes up by hand: Play's API cannot create a release for
   an app that has none. Take `play-bundle` from a Release run (Actions →
   Release → Run workflow), and upload it to Internal testing.
4. Google Cloud: enable the Google Play Android Developer API, create a
   service account, and make it a JSON key. Play Console → Users and
   permissions: invite the account's address, with release rights on this
   app only. Secret: `PLAY_SERVICE_ACCOUNT_JSON`, the whole JSON file.
