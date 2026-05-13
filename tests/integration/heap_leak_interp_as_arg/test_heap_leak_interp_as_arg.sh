#!/bin/sh
# Regression: AST_STRING_INTERP subexpression in argument position
# to a heap-classified outer call. Pre-fix the arg-temp wrapping in
# codegen_expr.c only captured AST_FUNCTION_CALL subexpressions;
# interp results landed inline with no temp to capture them and
# leaked per call. Post-fix the wrapping extends to AST_STRING_INTERP
# and `arg_drain_lookup` substitutes the temp in the outer emission.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] heap_leak_interp_as_arg: ae not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

if ! "$AE" build probe.ae -o "$tmpdir/probe" >"$tmpdir/build.log" 2>&1; then
    echo "  [FAIL] heap_leak_interp_as_arg: build failed"
    sed 's/^/    /' "$tmpdir/build.log" | head -30
    exit 1
fi

if ! "$tmpdir/probe" >"$tmpdir/run.log" 2>&1; then
    echo "  [FAIL] heap_leak_interp_as_arg: probe exited non-zero"
    sed 's/^/    /' "$tmpdir/run.log" | head -20
    exit 1
fi

if ! grep -q "all cases passed" "$tmpdir/run.log"; then
    echo "  [FAIL] heap_leak_interp_as_arg: probe output missing success marker"
    sed 's/^/    /' "$tmpdir/run.log" | head -20
    exit 1
fi

echo "  [PASS] heap_leak_interp_as_arg: 100000 interp-as-arg calls, RSS bounded"
exit 0
