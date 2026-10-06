#!/usr/bin/env bash
# Runs every function of the named Qt Test executables one at a time,
# printing each exit code. For CI, when a suite dies before its output is
# flushed and ctest shows nothing.
#
#   scripts/ci-diagnose-tests.sh build/ci/tst_crypto.exe ...
for exe in "$@"; do
    echo "== $exe"
    for fn in $("$exe" -functions 2>/dev/null | tr -d '\r' | sed 's/()$//'); do
        log=$(mktemp)
        "$exe" "$fn" > "$log" 2>&1
        code=$?
        printf '%-40s exit=%d (0x%X)\n' "$fn" "$code" "$code"
        [[ $code -ne 0 ]] && tail -30 "$log"
        rm -f "$log"
    done
done
exit 0
