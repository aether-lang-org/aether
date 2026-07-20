#!/bin/sh
# Imported-module bitstruct emission (asks/imported-module-bitstruct-emission.md).
#
# A `bitstruct` declared in an imported Aether module was referenced by the
# consumer's generated C (in accessor prototypes/signatures) but its backing
# typedef was never emitted, because the module-merge pass cloned imported
# `struct` definitions into the consumer's program AST but not `bitstruct`
# ones. Result: `error: unknown type name 'PropertyFlags'` — the Aether
# program failed to build even though the code was valid.
#
# Fix: module_merge_into_program clones AST_BITSTRUCT_DEFINITION into the
# consumer's program (bare name, like structs/distinct types), so the
# typechecker's resolve_bitstruct_types registers it and codegen emits its
# `typedef <backing> Name;` before any prototype references it.
#
# Fixture: lib/mqtypes.ae owns `bitstruct PropertyFlags : uint32_t` and
# exposes accessors; main.ae imports it and reads packed words.
#
# Acceptance: compiles, links, runs, prints "2", "21", "1".

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] bitstruct_cross_module: program errored (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$got" != "2 21 1" ]; then
    echo "  [FAIL] bitstruct_cross_module: expected '2 21 1', got '$got'"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] bitstruct_cross_module: imported bitstruct typedef emitted, accessors work"
exit 0
