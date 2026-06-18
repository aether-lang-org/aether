#!/bin/sh
# Regression (fbs-core ask #2): `expr!` unwrap-or-trap on a (value, err)
# tuple. probe.ae covers the success path; probe_panic.ae must abort with
# a non-zero exit when the error slot is non-empty.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

# ---- success path ----
if ! "$ROOT/build/ae" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] tuple_unwrap_bang: success-probe build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -20
    exit 1
fi

if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] tuple_unwrap_bang: success-probe exited non-zero"
    sed 's/^/    /' "$TMPDIR/run.log" | head -20
    exit 1
fi

if grep -q "tuple unwrap tests skipped" "$TMPDIR/run.log"; then
    echo "  [PASS] tuple_unwrap_bang: skipped (no OpenSSL backend)"
    exit 0
fi

if ! grep -q "PASS: tuple unwrap success path" "$TMPDIR/run.log"; then
    echo "  [FAIL] tuple_unwrap_bang: success path didn't reach PASS"
    sed 's/^/    /' "$TMPDIR/run.log" | head -20
    exit 1
fi

# ---- panic path ----
if ! "$ROOT/build/ae" build "$SCRIPT_DIR/probe_panic.ae" -o "$TMPDIR/panic" \
        >"$TMPDIR/build_panic.log" 2>&1; then
    echo "  [FAIL] tuple_unwrap_bang: panic-probe build failed"
    sed 's/^/    /' "$TMPDIR/build_panic.log" | head -20
    exit 1
fi

# Expect a NON-zero exit (panic/abort). If it exits 0 or prints the
# unreachable sentinel, the trap didn't fire.
panic_out="$("$TMPDIR/panic" 2>&1)"
panic_rc=$?
if [ "$panic_rc" -eq 0 ]; then
    echo "  [FAIL] tuple_unwrap_bang: panic path exited 0 (trap did not fire)"
    echo "$panic_out" | sed 's/^/    /' | head -10
    exit 1
fi
if echo "$panic_out" | grep -q "REACHED-AFTER-UNWRAP"; then
    echo "  [FAIL] tuple_unwrap_bang: code after unwrap ran (trap did not fire)"
    exit 1
fi

echo "  [PASS] tuple_unwrap_bang: success yields slot 0; error slot traps"
exit 0
