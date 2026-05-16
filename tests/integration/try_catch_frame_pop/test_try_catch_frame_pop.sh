#!/bin/sh
# Issue #501 regression suite: try { } catch { } codegen must not
# leak panic frames on non-local exits from the try body.
#
# Three programs covering:
#   1. return inside try body — main repro
#   2. break inside try body that's inside a loop — drain inside-loop
#      frames only
#   3. break inside a loop that's inside a try body — must NOT drain
#      the outer try frame
#
# Each program runs 100 iterations.  Pre-fix, AETHER_PANIC_MAX_DEPTH
# (32) was reached and the runtime aborted with
# `aether: try/catch nesting exceeded 32 — aborting`.  Post-fix,
# each program prints its expected success line and exits 0.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

run_case() {
    src="$1"
    expected="$2"
    label="$3"
    bin="$TMPDIR/$(basename "$src" .ae)"
    out="$TMPDIR/$(basename "$src" .ae).out"
    err="$TMPDIR/$(basename "$src" .ae).err"

    if ! AETHER_HOME="$ROOT" "$AE" build "$src" -o "$bin" >/dev/null 2>"$err"; then
        echo "  [FAIL] $label: ae build failed"
        cat "$err" | head -10 | sed 's/^/    /'
        return 1
    fi

    if ! "$bin" >"$out" 2>"$err"; then
        echo "  [FAIL] $label: program returned non-zero"
        echo "    stdout:" ; cat "$out" | sed 's/^/      /'
        echo "    stderr:" ; cat "$err" | sed 's/^/      /'
        return 1
    fi

    if ! grep -qF "$expected" "$out"; then
        echo "  [FAIL] $label: expected output not found"
        echo "    expected: $expected"
        echo "    got:" ; cat "$out" | sed 's/^/      /'
        return 1
    fi

    # Defensive: the pre-fix abort prints this on stderr.  If we ever
    # regress, the program may still exit 0 (if iter < 32) but
    # stderr would still carry the diagnostic from a previous run.
    if grep -q "try/catch nesting exceeded" "$err"; then
        echo "  [FAIL] $label: runtime hit the nesting cap (frame leak)"
        cat "$err" | head -5 | sed 's/^/    /'
        return 1
    fi
}

run_case "$SCRIPT_DIR/return_in_try.ae"     "survived 100 calls"      "case 1 (return in try)"
run_case "$SCRIPT_DIR/break_in_try.ae"      "break-in-try OK"         "case 2 (break in try)"
run_case "$SCRIPT_DIR/break_outside_try.ae" "break-outside-try OK"    "case 3 (break-not-leaving-try)"

echo "  [PASS] try/catch frame-pop on non-local exits (issue #501)"
