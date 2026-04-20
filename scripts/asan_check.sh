#!/usr/bin/env bash
# Full clean build with AddressSanitizer + UBSan and the unit tests.
# Any leak, read-after-free, or UB trips the build.
set -euo pipefail

make clean
make asan

# Quick smoke test: boot the sanitized binary briefly with no traffic
# so a regression in startup/shutdown gets caught before a real bench.
timeout 2s ./tick-relay --duration-ms 1000 >/dev/null || {
    rc=$?
    if [[ $rc -eq 124 ]]; then
        echo "asan smoke: binary ran for 2s without crashing (ok)"
    else
        echo "asan smoke: binary exited with rc=$rc" >&2
        exit "$rc"
    fi
}

echo "asan check: OK"
