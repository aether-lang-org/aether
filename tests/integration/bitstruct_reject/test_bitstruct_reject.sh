#!/bin/sh
# Issue #1132: bitstruct diagnostics. Each reject_*.ae is an ill-formed bitstruct
# that MUST fail to build, with a diagnostic that actually names the problem —
# a bitstruct exists to make a packed layout *exact*, so every way of getting the
# layout wrong has to be caught at compile time rather than silently miscompiled.
# The accepted shapes are covered by the regression test test_bitstruct.ae.
# Pruned from the generic .ae runner (these are expected-to-fail sources).
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
if [ ! -x "$AE" ]; then echo "  [SKIP] bitstruct_reject: $AE not built"; exit 0; fi
TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

# $1 = source basename, $2 = extended regex the diagnostic must match
check_rejected() {
    name="$1"
    want="$2"
    if "$AE" build "$SCRIPT_DIR/$name.ae" -o "$TMPDIR/out" >"$TMPDIR/$name.log" 2>&1; then
        echo "  [FAIL] bitstruct_reject: $name.ae compiled (should be rejected)"; exit 1
    fi
    if ! grep -qiE "$want" "$TMPDIR/$name.log"; then
        echo "  [FAIL] bitstruct_reject: $name.ae rejected, but not for the stated reason"
        echo "         expected a diagnostic matching: $want"
        sed 's/^/    /' "$TMPDIR/$name.log" | head -8; exit 1
    fi
}

check_rejected reject_no_backing     'requires an explicit backing type'
check_rejected reject_signed_backing 'backing type must be uint'
check_rejected reject_overlap        'overlap'
check_rejected reject_overflow       'do not fit in the 8-bit backing type'
check_rejected reject_wide_bool      'bool bitstruct field must be exactly one bit'
check_rejected reject_unknown_field  'not a field of bitstruct'
check_rejected reject_implicit_int   'mismatch|expected|cannot|incompatible'

echo "  [PASS] bitstruct_reject: missing/signed backing, overlap, overflow, wide bool, unknown field, implicit int — all rejected"
exit 0
