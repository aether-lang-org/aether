#!/bin/sh
# Regression (#914): `match` over a sum type is checked for exhaustiveness, and
# an arm naming a non-variant is rejected — both at `ae check` time, not left
# to a runtime fall-through or a raw gcc error. A complete match must still
# pass (guards against an over-broad rejection).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] sum_exhaustiveness: ae not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
cd "$SCRIPT_DIR" || exit 1
fail=0

# 1. non-exhaustive match -> must be rejected, naming the missing variant.
"$AE" check nonexhaustive.ae >"$tmpdir/ne.log" 2>&1
if [ $? -eq 0 ]; then
    echo "  [FAIL] sum_exhaustiveness: a non-exhaustive match passed ae check"
    fail=1
fi
if ! grep -qi 'non-exhaustive' "$tmpdir/ne.log"; then
    echo "  [FAIL] sum_exhaustiveness: missing the 'non-exhaustive' diagnostic"
    sed 's/^/        /' "$tmpdir/ne.log" | head -6
    fail=1
fi

# 2. arm naming a non-variant -> must be rejected.
"$AE" check bad_variant.ae >"$tmpdir/bv.log" 2>&1
if [ $? -eq 0 ]; then
    echo "  [FAIL] sum_exhaustiveness: a bad variant name passed ae check"
    fail=1
fi
if ! grep -qi 'not a variant' "$tmpdir/bv.log"; then
    echo "  [FAIL] sum_exhaustiveness: missing the 'not a variant' diagnostic"
    sed 's/^/        /' "$tmpdir/bv.log" | head -6
    fail=1
fi

# 3. complete match -> must pass.
if ! "$AE" check ok.ae >"$tmpdir/ok.log" 2>&1; then
    echo "  [FAIL] sum_exhaustiveness: a complete match was rejected"
    sed 's/^/        /' "$tmpdir/ok.log" | head -6
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "  [PASS] sum_exhaustiveness: non-exhaustive + bad-variant rejected; complete match accepted"
fi
exit $fail
