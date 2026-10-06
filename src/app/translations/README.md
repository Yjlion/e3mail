# Translations

One Qt Linguist file per language: `e3mail_<lang>.ts`. `e3mail_en.ts` holds
only English plural forms; English is the source language.

| Code | Language | State |
|---|---|---|
| de | German | machine translation, awaiting review |
| fr | French | machine translation, awaiting review |
| es | Spanish | machine translation, awaiting review |
| zh_CN | Chinese (Simplified) | machine translation, awaiting review |
| ja | Japanese | machine translation, awaiting review |
| ru | Russian | machine translation, awaiting review |
| pl | Polish | machine translation, awaiting review |
| ar | Arabic (right to left) | machine translation, awaiting review |
| he | Hebrew (right to left) | machine translation, awaiting review |
| yi | Yiddish, YIVO spelling (right to left) | machine translation, awaiting review |

When a native speaker has reviewed a language, change its row here and remove
nothing else: Settings tells every user that translations are machine-made
until that sentence is changed.

## Workflow

- After changing user-visible strings:
  `cmake --build --preset dev --target update_translations`. New entries come
  in unfinished.
- Translate with Qt Linguist (`linguist6 e3mail_de.ts`) or any XLIFF/TS
  editor. Keep `%1`…`%9`, `%n`, `<b>…</b>` and leading and trailing line
  breaks; the order of placeholders may change.
- `scripts/check-translations.py` (the `translations` test) fails on anything
  unfinished, empty, or with placeholders that differ from the source.
- Plurals: every string with `%n` has the language's own forms (one for
  Chinese and Japanese, three for Russian and Polish, six for Arabic).
- Look at the result: `scripts/screenshots.sh`, or `e3mail --lang <code>`.

## Choices

- `Re:` and `Fwd:` stay in English: a translated prefix breaks threading in the
  recipient's client. The reply and forward attribution lines are translated.
- Diagnostics from mail servers (`IMAP login failed: …`) stay as the server
  and protocol wrote them.
- Badges such as "PREVIEW" are short on purpose; a long translation is elided.
