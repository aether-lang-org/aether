#!/bin/sh
# Issue #924: first-class re-export. A module may list, in its `exports`, a
# symbol it brought in via `import` from another module — and that symbol
# becomes part of its own qualified surface, identically to one it defined.
# Previously `exports (X)` for an imported X failed at the consumer with
# `module 'M' has no export 'X'`, forcing facade monoliths and per-consumer
# extern re-declaration.
#
# Fixture (lib/):
#   leaf  — defines const LEAF_C and fn leaf_double
#   mid   — imports leaf; defines mid_base; re-exports leaf_double + LEAF_C
#   hub   — bodyless facade; re-exports mid_base, leaf_double, LEAF_C
# main imports ONLY hub and reaches all three through it:
#   - a function re-exported transitively (hub -> mid -> leaf)
#   - a const re-exported transitively
#   - a function re-exported one hop (hub -> mid)
#
# Acceptance: compiles, links, runs, and prints "10 7 100".

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?

if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] module_reexport: program errored (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

# Take the last non-empty line (skip any build chatter).
last=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tail -1)
if [ "$last" != "10 7 100" ]; then
    echo "  [FAIL] module_reexport: expected '10 7 100', got '$last'"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] module_reexport: re-exported fns + consts resolve through the facade"
exit 0
