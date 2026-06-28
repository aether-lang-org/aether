#!/bin/sh
# Issue #943 (closure analogue of #940): a bare top-level function used as an
# `fn` value INSIDE a trailing-block closure body failed to compile — the
# emitted closure function (`_closure_fn_0`) referenced an
# `_aether_bare_adapter_<name>` shim that was only defined AFTER the closure,
# so it was undeclared in the closure's translation unit.
#
# Cause: emit order. Closure bodies were emitted before the bare-fn adapters,
# but a closure body can itself wrap a bare fn. Fix emits adapter forward
# declarations before the closures (bodies still come after).
#
# Acceptance — probe.ae prints exactly:
#   top 42       (top-level control — already worked)
#   closure 42   (zero-arg bare fn inside a closure — the #943 case)
#   closure2 7   (bare fn with params inside a closure)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( AETHER_HOME="" "$AE" run "$SCRIPT_DIR/probe.ae" 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] bare_fn_in_closure: program errored (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' '|' | sed 's/|$//')
if [ "$got" != "top 42|closure 42|closure2 7" ]; then
    echo "  [FAIL] bare_fn_in_closure: expected 'top 42 / closure 42 / closure2 7', got:"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] bare_fn_in_closure: bare fn as fn-value inside a closure resolves its adapter"
exit 0
