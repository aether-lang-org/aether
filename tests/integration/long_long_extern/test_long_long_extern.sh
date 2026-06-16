#!/bin/sh
# Regression: extern `long long` parameter / return type emits the
# verbatim C spelling (matches a libc header that uses `long long`,
# avoids the int64_t conflicting-types error). Four-case matrix in
# probe.ae.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! "$ROOT/build/ae" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        --extra "$SCRIPT_DIR/shim.c" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] long_long_extern: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -15
    exit 1
fi

if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] long_long_extern: probe exited non-zero"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

if grep -q "^OK long_long_extern" "$TMPDIR/run.log"; then
    echo "  [PASS] long_long_extern: 4 cases"
else
    echo "  [FAIL] long_long_extern: didn't reach OK line"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi
