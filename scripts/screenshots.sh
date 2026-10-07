#!/usr/bin/env bash
# Regenerates screenshots/ from the fixture mailbox (tests/make_fixtures.cpp),
# never from a real mailbox. Renders offscreen; needs a dev build.
#
#   scripts/screenshots.sh [build-dir]
set -euo pipefail
BUILD=${1:-build/dev}
OUT=screenshots
DATA=$(mktemp -d)
EMPTY=$(mktemp -d)
trap 'rm -rf "$DATA" "$EMPTY"' EXIT
export QT_QPA_PLATFORM=offscreen E3MAIL_NO_KEYCHAIN=1 QT_LOGGING_RULES="e3.*.info=false"
APP="$BUILD/src/app/e3mail"
mkdir -p "$OUT"
"$BUILD/tests/e3mail-fixtures" "$DATA" >/dev/null
shot() { local name=$1; shift; "$APP" --data-dir "$DATA" --grab "$OUT/$name.png" "$@"; echo "$OUT/$name.png"; }
shot inbox
shot reading --open inbox:0
shot thread --open inbox:1
shot unverified --open unverified
shot composer --page compose
shot reply-html --open inbox:0 --page reply
shot contacts --page contacts
shot settings --page settings
# Other languages: right to left, CJK, and a long-worded one. CJK needs a
# CJK font installed (e.g. noto-fonts-cjk or wqy-microhei).
shot inbox-ar --lang ar
shot reading-ja --open inbox:0 --lang ja
shot settings-de --page settings --lang de
shot unverified-he --open unverified --lang he
"$APP" --data-dir "$EMPTY" --grab "$OUT/first-run.png"; echo "$OUT/first-run.png"
