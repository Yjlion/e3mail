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
# Granted here, so that Android's question does not cover the window.
adb shell pm grant "$PKG" android.permission.POST_NOTIFICATIONS || true
adb logcat -c
adb shell am start -W -n "$PKG/.E3Activity"
# Qt loads its libraries and the QML before the first frame. Wait, up to two
# minutes, for the middle of the screen to hold more than a few colours: an
# empty or black window has one, text and controls have many.
colours() { # <raw screencap>: distinct colours in the middle half, sampled
    python3 - "$1" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read()
w, h = struct.unpack("<II", d[:8])
off = len(d) - w * h * 4  # the header's length differs between versions
seen = set()
for y in range(h // 4, 3 * h // 4, 7):
    row = off + y * w * 4
    for x in range(0, w, 7):
        seen.add(d[row + 4 * x:row + 4 * x + 3])
print(len(seen))
PY
}
drawn=
for _ in $(seq 24); do
    sleep 5
    adb exec-out screencap > "$OUT/screen.raw"
    if [[ $(colours "$OUT/screen.raw") -gt 16 ]]; then drawn=1; break; fi
done
rm -f "$OUT/screen.raw"
adb exec-out screencap -p > "$OUT/screen.png"
PID=$(adb shell pidof "$PKG" | tr -d '\r' || true)
adb logcat -d > "$OUT/logcat.txt"

if [[ -z "$PID" ]]; then
    echo "e3mail is not running"
    grep -E "AndroidRuntime|DEBUG|libc|e3mail|Qt" "$OUT/logcat.txt" | tail -60
    exit 1
fi
if [[ -z "$drawn" ]]; then
    adb shell dumpsys window windows > "$OUT/windows.txt" || true
    adb shell dumpsys SurfaceFlinger --list > "$OUT/layers.txt" || true
    echo "e3mail runs but drew nothing in two minutes (screen.png, windows.txt)"
    exit 1
fi
# Qt logs under the application's name; QML errors read as in app_smoke.
if grep -E "qml: |\.qml:[0-9]+|ReferenceError|TypeError|Cannot assign|is not a type" "$OUT/logcat.txt"; then
    echo "QML errors (above)"
    exit 1
fi
echo "e3mail is running (pid $PID)"
