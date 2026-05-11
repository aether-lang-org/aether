#!/bin/sh
# Regression: closes the perf regression introduced by 0.149.0's
# escape-marked-LHS-alias-clear (commit 13e8652) when the LHS is the
# function's eventual return value. See probe.ae for full diagnosis.
#
# Builds the probe, runs it; the probe measures its own RSS growth
# across 500 iterations of a heap-accumulator return shape. Bounds
# the growth so the test discriminates the leak (pre-fix ~770 KB
# growth) from the post-fix flat-RSS shape (~0 KB).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] heap_tracker_return_escape_no_leak: ae not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

if ! "$AE" build probe.ae -o "$tmpdir/probe" >"$tmpdir/build.log" 2>&1; then
    echo "  [FAIL] heap_tracker_return_escape_no_leak: build failed"
    sed 's/^/    /' "$tmpdir/build.log" | head -30
    exit 1
fi

if ! "$tmpdir/probe" >"$tmpdir/run.log" 2>&1; then
    echo "  [FAIL] heap_tracker_return_escape_no_leak: probe exited non-zero"
    sed 's/^/    /' "$tmpdir/run.log" | head -20
    exit 1
fi

if ! grep -q "all cases passed" "$tmpdir/run.log"; then
    echo "  [FAIL] heap_tracker_return_escape_no_leak: probe output missing success marker"
    sed 's/^/    /' "$tmpdir/run.log" | head -20
    exit 1
fi

echo "  [PASS] heap_tracker_return_escape_no_leak: 500 return-escape accumulator calls, RSS bounded"
exit 0
