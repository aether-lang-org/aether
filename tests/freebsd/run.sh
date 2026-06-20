#!/bin/sh
# tests/freebsd/run.sh — run the FreeBSD-only sandbox enforcement tests.
#
# These exercise REAL Capsicum/Casper behaviour and can only pass on a
# FreeBSD host with a Capsicum-enabled kernel; there is no FreeBSD runner
# in the CI matrix yet, so they live outside tests/regression/ and are
# driven by this script (locally / by the nightly harness).
#
# Each test .ae is a self-contained program: exit 0 = pass, 1 = fail,
# 2 = skip (not FreeBSD). On a non-FreeBSD host every test skips and the
# script still exits 0 — so it's safe to invoke anywhere.
#
# Usage:  sh tests/freebsd/run.sh        (uses ./build/ae)
#         AE=/path/to/ae sh tests/freebsd/run.sh
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
AE="${AE:-$ROOT/build/ae}"

if [ ! -x "$AE" ]; then
    echo "ERROR: ae not found/executable at $AE (build it first: gmake ae stdlib)" >&2
    exit 3
fi

echo "FreeBSD sandbox enforcement tests — $(uname -srm)"
echo "ae: $AE"
echo

pass=0 fail=0 skip=0 failed_list=""
for t in "$ROOT"/tests/freebsd/*.ae; do
    name=$(basename "$t" .ae)
    printf '  [ .. ] %s\n' "$name"
    out=$("$AE" run "$t" 2>&1)
    rc=$?
    case "$rc" in
        0) echo "  [PASS] $name"; pass=$((pass+1)) ;;
        2) echo "  [SKIP] $name (not FreeBSD)"; skip=$((skip+1)) ;;
        *) echo "  [FAIL] $name (exit $rc)"; echo "$out" | sed 's/^/        /'
           fail=$((fail+1)); failed_list="$failed_list $name" ;;
    esac
done

echo
echo "FreeBSD sandbox tests: $pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ] || { echo "FAILED:$failed_list"; exit 1; }
exit 0
