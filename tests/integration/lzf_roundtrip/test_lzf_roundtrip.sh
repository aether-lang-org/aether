#!/bin/sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! "$ROOT/build/ae" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] lzf_roundtrip: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -20
    exit 1
fi

if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] lzf_roundtrip: probe exited non-zero"
    sed 's/^/    /' "$TMPDIR/run.log" | head -40
    exit 1
fi

if grep -q "All std.lzf tests passed" "$TMPDIR/run.log"; then
    echo "  [PASS] lzf_roundtrip: 5 cases"
else
    echo "  [FAIL] lzf_roundtrip: didn't reach final PASS line"
    sed 's/^/    /' "$TMPDIR/run.log" | head -40
    exit 1
fi
