#!/bin/sh
# Regression: std.cryptography incremental digest context (fbs-core ask
# #4) — digest_new / digest_update / digest_final_hex / digest_final_bytes
# / digest_free. Streamed digest must equal the one-shot digest and the
# known vectors. See probe.ae for the six-case matrix.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! "$ROOT/build/ae" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] cryptography_digest_stream: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -15
    exit 1
fi

if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] cryptography_digest_stream: probe exited non-zero"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

if grep -q "All streaming digest tests passed" "$TMPDIR/run.log"; then
    echo "  [PASS] cryptography_digest_stream: 6 cases"
elif grep -q "streaming digest tests skipped" "$TMPDIR/run.log"; then
    reason=$(grep '^SKIP cryptography_digest_stream:' "$TMPDIR/run.log" | head -1)
    echo "  [PASS] cryptography_digest_stream: ${reason:-skipped (no OpenSSL backend)}"
else
    echo "  [FAIL] cryptography_digest_stream: didn't reach final PASS or SKIP line"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi
