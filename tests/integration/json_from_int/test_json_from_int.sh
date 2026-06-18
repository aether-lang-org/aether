#!/bin/sh
# Regression: std.json.from_int constructor (sibling of json.num) —
# integers serialise as bare integers, full int64 range, no scientific
# notation crossover at ~1e7. Seven-case matrix in probe.ae.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! "$ROOT/build/ae" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] json_from_int: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -15
    exit 1
fi

if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] json_from_int: probe exited non-zero"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

if grep -q "^OK json_from_int" "$TMPDIR/run.log"; then
    echo "  [PASS] json_from_int: 7 cases"
else
    echo "  [FAIL] json_from_int: didn't reach OK line"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi
