#!/bin/sh
# A module whose function body declares a local variable with the SAME name as
# the module itself (`flags` module + `flags` local). The member-access
# typechecker took its namespace-qualified-constant branch whenever the base
# identifier matched a visible namespace, with no precedence for a same-named
# in-scope value — so the module's own `flags.count` field access was
# mis-resolved as a `flags_count` const lookup and failed with
# `module 'flags' has no export 'count'`.
#
# Fix (typechecker.c AST_MEMBER_ACCESS): skip the namespace branch when the
# base identifier resolves to a value symbol in scope, so a local shadows the
# namespace and struct-field resolution wins. Not bitstruct-specific
# (reproduces with a plain struct); surfaced by the imported-bitstruct ask
# whose verbatim repro named both the module and a local `flags`.
#
# Acceptance: compiles, links, runs, prints "7".

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] module_name_local_shadow: program errored (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$got" != "7" ]; then
    echo "  [FAIL] module_name_local_shadow: expected '7', got '$got'"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] module_name_local_shadow: local var shadows same-named module namespace"
exit 0
