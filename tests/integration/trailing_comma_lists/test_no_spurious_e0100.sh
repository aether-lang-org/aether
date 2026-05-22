#!/bin/sh
# Regression guard for Blocker 1 of closures-with-context-builder-blockers.md:
# a trailing comma in an `exports (…)` list (or a selective import list)
# must NOT produce a spurious `error[E0100]: Expected IDENTIFIER, got
# RIGHT_PAREN`.
#
# This needs a dedicated shell test rather than a plain .ae test because
# the buggy compiler still *exited 0* — it recovered from the parse error
# and emitted a usable program — so an exit-code-only check passed even
# when broken. The deterministic signal is the E0100 line printed to
# stderr. The pre-fix compiler printed it on every run; the fixed
# compiler prints nothing.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

pass=0
fail=0

cd "$SCRIPT_DIR"
err=$("$ROOT/build/ae" build test_trailing_comma_lists.ae -o /tmp/ae_tcl_guard 2>&1)
rc=$?

if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] build of trailing-comma program failed (exit $rc)"
    echo "$err" | head -5
    fail=$((fail + 1))
else
    echo "  [PASS] build succeeded"
    pass=$((pass + 1))
fi

if echo "$err" | grep -q "E0100"; then
    echo "  [FAIL] spurious E0100 emitted on a trailing comma:"
    echo "$err" | grep "E0100"
    fail=$((fail + 1))
else
    echo "  [PASS] no spurious E0100"
    pass=$((pass + 1))
fi

rm -f /tmp/ae_tcl_guard

echo ""
echo "Trailing-comma E0100 guard: $pass passed, $fail failed"
if [ "$fail" -gt 0 ]; then exit 1; fi
