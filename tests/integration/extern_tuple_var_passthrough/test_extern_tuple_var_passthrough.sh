#!/bin/sh
# Tests #1062: a tuple-typed VALUE (a variable, or the result of a
# tuple-returning extern) is accepted at a tuple-typed extern parameter, not
# only a parenthesized tuple literal. The follow-up to #1033/#1040. The sibling
# shim.c defines C functions taking/returning struct-by-value with layouts
# matching the codegen-synthesized `_tuple_*` typedefs; ae build auto-links it.
#
# The probe round-trips a struct through a variable and a direct call chain and
# asserts the values crossed the FFI boundary intact. Also asserts the guards:
# a shape mismatch and a non-tuple value at a tuple param must fail typecheck.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

build_log="$tmpdir/build.log"
if ! "$AE" build probe.ae -o "$tmpdir/probe" >"$build_log" 2>&1; then
    echo "  [FAIL] extern_tuple_var_passthrough: ae build failed"
    sed 's/^/    /' "$build_log" | head -20
    exit 1
fi

if ! "$tmpdir/probe" > "$tmpdir/run.out" 2>&1 || ! grep -q "^OK$" "$tmpdir/run.out"; then
    echo "  [FAIL] extern_tuple_var_passthrough: binary ran but did not print OK"
    sed 's/^/    /' "$tmpdir/run.out" | head -10
    exit 1
fi

# Guard 1: a shape-mismatched tuple value is a TYPECHECK error, not a gcc error.
cat > "$tmpdir/badshape.ae" <<'AE'
extern mk2() -> (ptr, int)
extern use3(t: (ptr, int, int)) -> int
main() { x = mk2(); r = use3(x) }
AE
if "$AE" check "$tmpdir/badshape.ae" >"$tmpdir/badshape.log" 2>&1; then
    echo "  [FAIL] extern_tuple_var_passthrough: (ptr,int) value into (ptr,int,int) param passed typecheck"
    exit 1
fi
if ! grep -q "tuple-typed" "$tmpdir/badshape.log"; then
    echo "  [FAIL] extern_tuple_var_passthrough: shape-mismatch error lacks the tuple diagnostic:"
    sed 's/^/    /' "$tmpdir/badshape.log" | head -5
    exit 1
fi

# Guard 2: a non-tuple value at a tuple param is a TYPECHECK error.
cat > "$tmpdir/badkind.ae" <<'AE'
extern use3b(t: (ptr, int, int)) -> int
main() { y = 5; r = use3b(y) }
AE
if "$AE" check "$tmpdir/badkind.ae" >"$tmpdir/badkind.log" 2>&1; then
    echo "  [FAIL] extern_tuple_var_passthrough: int value into tuple param passed typecheck"
    exit 1
fi

echo "  [PASS] extern_tuple_var_passthrough: tuple values (vars + call chains) cross the FFI by value; mismatches rejected"
exit 0
