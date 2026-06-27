#!/bin/sh
# Issue #940: passing a bare top-level function as an `fn`-typed argument
# ACROSS a module boundary failed to compile — the caller referenced an
# `_aether_bare_adapter_<name>` env-ignoring shim that was never emitted in
# the caller's translation unit (`_aether_bare_adapter_val' undeclared`).
#
# Cause: the adapter-discovery pre-walk looked up the call's callee by its
# AST name, which for a qualified `mod.fn(...)` call is still dotted
# (`runner.runit`) while the merged definition is `runner_runit`. The
# lookup missed, the fn-typed param was never inspected, and the adapter for
# the bare-fn argument was never registered/emitted. Same code single-file
# worked (callee is bare `runit`).
#
# Fixture (lib/runner.ae): runit(f: fn) and apply2(f: fn, a, b) — a library
# taking a caller-supplied callback by bare name. main.ae passes consumer
# functions `val` (zero-arg) and `add` (params + return) across the import.
#
# Acceptance: compiles, links, runs, prints "42" then "7".

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] bare_fn_cross_module: program errored (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$got" != "42 7" ]; then
    echo "  [FAIL] bare_fn_cross_module: expected '42 7', got '$got'"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] bare_fn_cross_module: bare fn passed as fn-arg across import resolves its adapter"
exit 0
