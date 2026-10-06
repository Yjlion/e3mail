# 0014 — Languages: ICU for charsets and dates, Qt Linguist for the interface

**Status:** Accepted — 2026-10-06

## Context
Mail arrives in every charset (ISO-2022-JP, Shift_JIS, GB18030, Big5, KOI8-R,
windows-1256…). Decoding went through `QStringDecoder`, which knows them only
when the Qt build has ICU; the official Qt binaries used by CI and the
packages do not promise that on every platform. The interface was English only,
with hand-written plurals, and is to ship in ten more languages, three of them
right to left.

## Decision
- **e3core links ICU** (uc, i18n, data) and decodes charsets with ICU's
  converters, by any of their aliases. UTF-8 and Windows-1252 keep their fast
  paths. A declared charset that does not fit the bytes falls back to UTF-8,
  then to the declared charset with replacement characters. Day-and-month
  dates come from CLDR through ICU's pattern generator
  (`LocalePatterns::monthDay`), since QLocale has no such format.
- **Qt Linguist** for the interface: `qsTr`/`tr`, and
  `QCoreApplication::translate` in the core; `%n` plurals with each language's
  forms; `.ts` files compiled into the binary. Languages: German, French,
  Spanish, Simplified Chinese, Japanese, Russian, Polish, Arabic, Hebrew,
  Yiddish. They are machine translations until reviewed, and the interface
  says so.
- The language is **per installation**, not per account (`app.ini` in the data
  directory), default "follow the system", switchable at runtime.
- **Right to left** by mirroring the layout (`LayoutMirroring` on the window;
  controls follow their locale), directional icons turned around, and mail
  text (subjects, senders) keeping its own direction at the layout's start
  edge.
- Not translated: `Re:`/`Fwd:` (threading in other clients), and protocol
  diagnostics from servers.

## Consequences
- ICU joins the dependencies on every platform: `libicu-dev`, Homebrew
  `icu4c` (keg-only, so `ICU_ROOT`), vcpkg `icu` (long to build; cached).
  Packages carry it: the AppImage and macOS bundle by their deploy tools, the
  Windows packages with the other vcpkg DLLs, the `.deb` as a dependency.
  Qt's own ICU, where it has one, is a separate copy; ICU's versioned symbols
  keep them apart.
- Text stored at receive time (the "could not be decrypted" notice) is in the
  language of that moment.
- A new string is a test failure until all ten languages have it.
