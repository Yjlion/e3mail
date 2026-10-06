# CLAUDE.md

Read [`docs/handoff.md`](docs/handoff.md) first. It says what is true right now,
including what is broken.

## What this is

e3mail is an end-to-end encrypted classic email client: a ground-up port of
[eeemail](https://github.com/Yjlion/eeemail)'s ideas to C++20 and Qt 6 (QML),
with no Delta Chat code. IMAP and POP3 are transport only, and the local
database is the mailbox. Unaudited, prerelease.

| Path | What it is |
|---|---|
| `src/core/` | `libe3core`, the whole engine (QtCore + QtNetwork only) |
| `src/app/` | the QML app: `controllers/`, `models/`, `qml/` |
| `src/cli/` | `e3mail-cli`, the same engine headless |
| `tests/` | Qt Test suites, `FakeMailServer`, `make_fixtures.cpp` |
| `server/compose/` | Postfix + Dovecot test server (project `e3mail-test`) |
| `scripts/` | `e2e.py` (live pass), `screenshots.sh`, `build-rnp.sh` |
| `docs/adr/` | decisions, one per file |

## Build and test

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

The `dev` preset builds with `-Werror`. Run everything the change touches,
which is usually the whole suite (about two seconds). Changes to QML: run
`scripts/screenshots.sh` and look at the images. The `app_smoke` test only
proves the QML loads. Changes to transports or TLS: run `scripts/e2e.py`
against `server/compose`.

Dependencies on Arch: `qt6-base qt6-declarative qt6-svg botan json-c sqlite`,
plus RNP from `scripts/build-rnp.sh /usr/local` (it is not packaged).

## Rules

- **ADRs are immutable.** Supersede them or add a dated amendment, and add a
  row to `docs/adr/README.md`.
- **`docs/handoff.md` records what each piece of work taught**, honestly.
- **Schema migrations are append-only** (`src/core/store/Schema.cpp`). Never
  edit one that has shipped.
- **Every user-visible change goes through `OpLog::record`** in the same
  transaction. Multi-client sync (Phase 8) depends on it.
- **Message content never reaches the network**: keep both barriers
  (ADR 0009).
- Commit subjects are imperative and sentence case, with no prefixes. The body
  explains why.

## Before running Docker

`docker compose` in `server/compose/` uses the project name `e3mail-test`. Keep
the `name:` line: without it, the project collides with any other
`server/compose/` checkout on the machine and replaces its containers.
