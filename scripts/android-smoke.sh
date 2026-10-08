#!/usr/bin/env bash
# Boots the app on a running emulator or device and fails on QML errors, like
# app_smoke does on the desktop. Leaves logcat.txt and screen.png in <out>.
#
#   scripts/android-smoke.sh <apk> [out-dir]
set -euo pipefail
APK=${1:?usage: android-smoke.sh <apk> [out-dir]}
OUT=${2:-.}
PKG=org.e3mail.e3mail
mkdir -p "$OUT"

adb wait-for-device
adb install -r "$APK"
adb logcat -c
adb shell am start -W -n "$PKG/.E3Activity"
# Qt loads its libraries and the QML before the first frame.
sleep 25
PID=$(adb shell pidof "$PKG" | tr -d '\r' || true)
adb logcat -d > "$OUT/logcat.txt"
adb exec-out screencap -p > "$OUT/screen.png"

if [[ -z "$PID" ]]; then
    echo "e3mail is not running"
    grep -E "AndroidRuntime|DEBUG|libc|e3mail|Qt" "$OUT/logcat.txt" | tail -60
    exit 1
fi
# Qt logs under the application's name; QML errors read as in app_smoke.
if grep -E "qml: |\.qml:[0-9]+|ReferenceError|TypeError|Cannot assign|is not a type" "$OUT/logcat.txt"; then
    echo "QML errors (above)"
    exit 1
fi
echo "e3mail is running (pid $PID)"
