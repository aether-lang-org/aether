#!/bin/sh
# Regression: cross-function + recursion + bare-identifier-return-of-
# heap-tracked-local. Pre-fix the classifier (walk_returns_for_heap_check)
# bare-identifier-return check queried gen->heap_string_vars in the
# CALLER's context (cross-fn classification triggered from outside the
# callee's own emission), missed the callee's local, AND-folded the
# function non-heap. Caller's wrapper set _heap_<lhs> = 0 → buffer
# leaked per call → O(N²) on avn's bench.
#
# Post-fix: classifier uses a self-sufficient body walk
# (body_assigns_var_from_heap) instead of gen->heap_string_vars — the
# lookup works regardless of which context invoked the classifier.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] heap_leak_cross_fn_recursion: ae not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

if ! "$AE" build probe.ae -o "$tmpdir/probe" >"$tmpdir/build.log" 2>&1; then
    echo "  [FAIL] heap_leak_cross_fn_recursion: build failed"
    sed 's/^/    /' "$tmpdir/build.log" | head -30
    exit 1
fi

if ! "$tmpdir/probe" >"$tmpdir/run.log" 2>&1; then
    echo "  [FAIL] heap_leak_cross_fn_recursion: probe exited non-zero"
    sed 's/^/    /' "$tmpdir/run.log" | head -20
    exit 1
fi

if ! grep -q "all cases passed" "$tmpdir/run.log"; then
    echo "  [FAIL] heap_leak_cross_fn_recursion: probe output missing success marker"
    sed 's/^/    /' "$tmpdir/run.log" | head -20
    exit 1
fi

echo "  [PASS] heap_leak_cross_fn_recursion: 2x1000 cross-fn+recursion calls, min-growth RSS bounded"
exit 0
