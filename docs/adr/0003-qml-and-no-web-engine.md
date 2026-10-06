# 0003 — The UI is Qt Quick, and mail never renders in a web engine

**Status:** Accepted — 2026-10-05

## Context
The user chose QML over Widgets. HTML mail has to be shown somehow, and the
obvious way, QtWebEngine, is a Chromium: it is large, hard to ship on all three
platforms, and a JavaScript engine pointed at the most hostile input a desktop
program receives.

## Decision
HTML mail is sanitized (ADR 0009) and rendered in a read-only QML `TextEdit`
with rich text. Qt's rich-text engine has no scripting, no forms and no
navigation. The UI is Qt Quick Controls with the Basic style and our own
theme, which follows the system's light or dark preference.

## Consequences
- Complex HTML newsletters render simplified. That is the trade.
- The packages carry no browser engine.
