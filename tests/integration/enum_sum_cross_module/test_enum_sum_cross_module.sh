#!/bin/sh
# Imported-module enum and sum-type emission — the enum/sum siblings of the
# imported-bitstruct fix (#1192). module_merge_into_program now clones
# AST_ENUM_DEFINITION and AST_SUM_TYPE_DEF from imported modules into the
# consumer's program AST (bare name, like structs/bitstructs), so the
# typechecker registers them (resolving `Enum.Member` selectors and match
# arms) and codegen emits their typedefs. Without this an imported enum used
# by name failed with `Undefined variable 'Color'`, and an imported sum type
# with `unknown type name 'Shape'` in the generated C.
#
# Fixture: lib/types/module.ae owns `enum Color`, `type Shape = Circle | Rect`,
# and accessors; main.ae imports it and calls them.
#
# Acceptance: compiles, links, runs, prints "1", "2".

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] enum_sum_cross_module: program errored (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$got" != "1 2" ]; then
    echo "  [FAIL] enum_sum_cross_module: expected '1 2', got '$got'"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] enum_sum_cross_module: imported enum + sum type typedefs emitted, accessors work"
exit 0
