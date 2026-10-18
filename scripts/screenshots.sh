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
export QT_QPA_PLATFORM=offscreen E3MAIL_NO_KEYCHAIN=1 E3MAIL_DEVICE_NAME=Laptop QT_LOGGING_RULES="e3.*.info=false"
APP="$BUILD/src/app/e3mail"
mkdir -p "$OUT"
"$BUILD/tests/e3mail-fixtures" "$DATA" >/dev/null
shot() { local name=$1; shift; "$APP" --data-dir "$DATA" --grab "$OUT/$name.png" "$@"; echo "$OUT/$name.png"; }
shot inbox
shot reading --open inbox:0
shot thread --open inbox:1
shot unverified --open unverified
shot composer --page compose
shot composer-formatting --page formatting
shot signature --page signature
shot devices --page devices
shot pair --page pair
shot reply-html --open inbox:0 --page reply
shot contacts --page contacts
shot source --open inbox:0 --page source
shot settings --page settings
# Other languages: right to left, CJK, and a long-worded one. CJK needs a
# CJK font installed (e.g. noto-fonts-cjk or wqy-microhei).
shot inbox-ar --lang ar
shot reading-ja --open inbox:0 --lang ja
shot settings-de --page settings --lang de
shot unverified-he --open unverified --lang he
shot source-ar --open inbox:0 --page source --lang ar
shot settings-ar --page settings --lang ar
shot signature-ar --page signature --lang ar
shot devices-ar --page devices --lang ar
shot backups --page backups
shot backups-ar --page backups --lang ar
shot phone-backups --page backups --size 400x820
# Set up, backed up once, into a folder.
REPO=$(mktemp -d)
"$BUILD/src/cli/e3mail-cli" --data-dir "$DATA" backup-setup --dest "{\"type\":\"folder\",\"path\":\"$REPO\"}" \
    --passphrase "screenshot passphrase" >/dev/null
"$BUILD/src/cli/e3mail-cli" --data-dir "$DATA" backup-now >/dev/null
shot backups-on --page backups
shot phone-backups-on-ar --page backups --lang ar --size 400x820
"$BUILD/src/cli/e3mail-cli" --data-dir "$DATA" backup-forget >/dev/null
rm -rf "$REPO"
shot pair-ar --page pair --lang ar
# Removed on another device while away.
"$BUILD/src/cli/e3mail-cli" --data-dir "$DATA" config p2p_removed_by Desktop >/dev/null
shot devices-removed --page devices
shot devices-removed-ar --page devices --lang ar
shot phone-devices-removed --page devices --size 400x820
"$BUILD/src/cli/e3mail-cli" --data-dir "$DATA" config p2p_removed_by "" >/dev/null
"$APP" --data-dir "$EMPTY" --grab "$OUT/first-run.png"; echo "$OUT/first-run.png"
"$APP" --data-dir "$EMPTY" --grab "$OUT/join.png" --page join; echo "$OUT/join.png"
# Reading the code with the camera; there is none offscreen, so the scan
# view says so.
export E3MAIL_SCREENSHOT_CAMERA=1
"$APP" --data-dir "$EMPTY" --grab "$OUT/join-scan-button.png" --page join; echo "$OUT/join-scan-button.png"
"$APP" --data-dir "$EMPTY" --grab "$OUT/scan.png" --page scan; echo "$OUT/scan.png"
"$APP" --data-dir "$EMPTY" --grab "$OUT/scan-ar.png" --page scan --lang ar; echo "$OUT/scan-ar.png"
# Signing in with Google (ADR 0019); offered only with a client ID.
export E3MAIL_OAUTH_GOOGLE_ID=screenshot.apps.googleusercontent.com
SEEN=$(mktemp -d) # first run already acknowledged, so the form shows
touch "$SEEN/first-run-acknowledged"
"$APP" --data-dir "$SEEN" --grab "$OUT/setup-oauth.png" --page setup-oauth; echo "$OUT/setup-oauth.png"
"$APP" --data-dir "$SEEN" --grab "$OUT/setup-oauth-ar.png" --page setup-oauth --lang ar; echo "$OUT/setup-oauth-ar.png"
"$APP" --data-dir "$SEEN" --grab "$OUT/phone-setup-oauth.png" --page setup-oauth --size 400x820; echo "$OUT/phone-setup-oauth.png"
"$APP" --data-dir "$SEEN" --grab "$OUT/restore.png" --page restore; echo "$OUT/restore.png"
"$APP" --data-dir "$SEEN" --grab "$OUT/phone-restore-ar.png" --page restore --lang ar --size 400x820; echo "$OUT/phone-restore-ar.png"
rm -rf "$SEEN"
unset E3MAIL_OAUTH_GOOGLE_ID
# A phone: one pane at a time.
PHONE=(--size 400x820)
shot phone-inbox "${PHONE[@]}"
shot phone-menu --page menu "${PHONE[@]}"
shot phone-reading --open inbox:0 "${PHONE[@]}"
shot phone-composer --page compose "${PHONE[@]}"
shot phone-contacts --page contacts "${PHONE[@]}"
shot phone-settings --page settings "${PHONE[@]}"
shot phone-signature --page signature "${PHONE[@]}"
shot phone-devices --page devices "${PHONE[@]}"
shot phone-pair --page pair "${PHONE[@]}"
shot phone-source --open inbox:0 --page source "${PHONE[@]}"
shot phone-inbox-ar --lang ar "${PHONE[@]}"
shot phone-reading-ar --open inbox:0 --lang ar "${PHONE[@]}"
shot phone-menu-ar --page menu --lang ar "${PHONE[@]}"
"$APP" --data-dir "$EMPTY" --grab "$OUT/phone-first-run.png" "${PHONE[@]}"; echo "$OUT/phone-first-run.png"
"$APP" --data-dir "$EMPTY" --grab "$OUT/phone-join-ar.png" --page join --lang ar "${PHONE[@]}"; echo "$OUT/phone-join-ar.png"
"$APP" --data-dir "$EMPTY" --grab "$OUT/phone-scan.png" --page scan "${PHONE[@]}"; echo "$OUT/phone-scan.png"
