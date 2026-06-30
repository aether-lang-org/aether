#!/bin/sh
# Regression (#952 part B): the type checker must reject over-/under-applying
# an extern (C FFI) function, instead of letting a raw gcc "too many arguments"
# error leak through `ae check`. The reported case: the zero-arg
# `math.deg_to_rad` constant called as `math.deg_to_rad(x)`.
#
# Guards (must NOT be rejected): a correct zero-arg call, and a variadic
# extern called with trailing args. (`_ctx`-first builder externs are covered
# by the std.host manifest integration test.)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] extern_arity_check: ae not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
cd "$SCRIPT_DIR" || exit 1

fail=0

# 1. Over-applying a zero-arg extern -> `ae check` MUST reject it.
"$AE" check over_apply.ae >"$tmpdir/over.log" 2>&1
if [ $? -eq 0 ]; then
    echo "  [FAIL] extern_arity_check: over-applied extern (math.deg_to_rad(x)) passed ae check"
    fail=1
fi
if ! grep -qi 'argument' "$tmpdir/over.log"; then
    echo "  [FAIL] extern_arity_check: no arity error reported for the over-applied extern"
    sed 's/^/        /' "$tmpdir/over.log" | head -6
    fail=1
fi

# 2. Correct zero-arg call -> MUST pass.
if ! "$AE" check correct.ae >"$tmpdir/ok.log" 2>&1; then
    echo "  [FAIL] extern_arity_check: a correct zero-arg extern call was rejected"
    sed 's/^/        /' "$tmpdir/ok.log" | head -6
    fail=1
fi

# 3. Variadic extern with trailing args -> MUST pass (no false positive).
if ! "$AE" check variadic.ae >"$tmpdir/var.log" 2>&1; then
    echo "  [FAIL] extern_arity_check: a variadic extern call was rejected"
    sed 's/^/        /' "$tmpdir/var.log" | head -6
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "  [PASS] extern_arity_check: over-applied extern rejected; correct + variadic calls accepted"
fi
exit $fail
