#!/bin/sh
# Regression: std.cryptography.random_hex + random_base64 convenience
# wrappers over the OS CSPRNG. Six-case matrix in probe.ae.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! "$ROOT/build/ae" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] cryptography_random_hex: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -15
    exit 1
fi

if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] cryptography_random_hex: probe exited non-zero"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

if grep -q "All random_hex / random_base64 tests passed" "$TMPDIR/run.log"; then
    echo "  [PASS] cryptography_random_hex: 6 cases"
else
    echo "  [FAIL] cryptography_random_hex: didn't reach final PASS line"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi
