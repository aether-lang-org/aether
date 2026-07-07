#!/bin/sh
# Tests extern declarations with tuple-typed PARAMETERS (#1033) — the
# by-value-struct-argument mirror of #271's tuple returns — plus the
# `f32` element type in both parameter and return position. The shim
# defines C functions taking struct-by-value with layouts matching the
# codegen-synthesized `_tuple_*` typedefs:
#   - (f32, f32) pairs           (raylib Vector2 shape)
#   - (byte, byte, byte, byte)   (raylib Color shape)
#   - mixed ptr + vector params  (ImageDrawTriangle-like)
# The probe passes parenthesized tuple literals at each call site and
# asserts the values crossed the FFI boundary intact.
#
# Also asserts the two guard errors: a tuple literal aimed at a
# non-tuple param, and an element-count mismatch, must fail typecheck
# (not gcc).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

build_log="$tmpdir/build.log"
if ! "$AE" build probe.ae -o "$tmpdir/probe" >"$build_log" 2>&1; then
    echo "  [FAIL] extern_tuple_param: ae build failed"
    sed 's/^/    /' "$build_log" | head -20
    exit 1
fi

if ! "$tmpdir/probe" > "$tmpdir/run.out" 2>&1 || ! grep -q "^OK$" "$tmpdir/run.out"; then
    echo "  [FAIL] extern_tuple_param: binary ran but did not print OK"
    sed 's/^/    /' "$tmpdir/run.out" | head -10
    exit 1
fi

# Guard 1: element-count mismatch is a TYPECHECK error.
cat > "$tmpdir/badcount.ae" <<'AE'
extern vec2_dot(a: (f32, f32), b: (f32, f32)) -> float
main() { d = vec2_dot((1.0, 2.0, 3.0), (3.0, 4.0)) }
AE
if "$AE" check "$tmpdir/badcount.ae" >"$tmpdir/badcount.log" 2>&1; then
    echo "  [FAIL] extern_tuple_param: 3-element literal for 2-element param passed typecheck"
    exit 1
fi
if ! grep -q "element" "$tmpdir/badcount.log"; then
    echo "  [FAIL] extern_tuple_param: count-mismatch error lacks element diagnostics:"
    sed 's/^/    /' "$tmpdir/badcount.log" | head -5
    exit 1
fi

# Guard 2: tuple literal into a non-tuple param is a TYPECHECK error.
cat > "$tmpdir/badtarget.ae" <<'AE'
extern take_int(v: int) -> int
main() { d = take_int((1, 2)) }
AE
if "$AE" check "$tmpdir/badtarget.ae" >"$tmpdir/badtarget.log" 2>&1; then
    echo "  [FAIL] extern_tuple_param: tuple literal into int param passed typecheck"
    exit 1
fi

echo "  [PASS] extern_tuple_param: f32/byte tuple params + f32 returns cross the FFI by value"
exit 0
