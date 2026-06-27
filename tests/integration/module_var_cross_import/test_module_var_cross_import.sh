#!/bin/sh
# Issue #937: a module-level mutable `var` (#701) must persist across the
# import boundary. Pre-fix, a write inside an imported module's function was
# lowered as a shadowing LOCAL (the merge's intra-module rename never renamed
# the assignment target to the prefixed global), so the store never reached
# the shared `static` — a later call read the zero-initializer back
# (write-returned=7, read-back=0).
#
# Fixture (lib/ambient.ae): an ambient cell set by init(), mutated by bump(),
# read by current(); plus scratch() which mixes a genuine LOCAL (tmp, must
# stay local) with a write to the global (cur, must reach the shared cell).
#
# Acceptance — main.ae prints exactly:
#   12   (init(10) + bump + bump, read back across the boundary)
#   100  (scratch(5): tmp=50 local, cur=50 global  -> 50+50)
#   50   (current() now sees the global write from scratch)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] module_var_cross_import: program errored (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$got" != "12 100 50" ]; then
    echo "  [FAIL] module_var_cross_import: expected '12 100 50', got '$got'"
    echo "       (a '0' read-back means the imported module-var write didn't persist)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] module_var_cross_import: imported module-var writes persist; locals stay local"
exit 0
