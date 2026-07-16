#!/bin/sh
# Phase 3 of error-unification: cross-module `fault` identity. Two modules
# (afs, anet) each declare a fault member named `NotFound`. Because a fault's
# interned content is its namespace-QUALIFIED name (`"afs.NotFound"` vs
# `"anet.NotFound"`), an error from afs resolves as `afs.NotFound` and does
# NOT collide with `anet.NotFound` — the module-merge pass prefixes each
# member's C symbol (`afs_NotFound`) and qualifies its content. Acceptance:
# compiles, links, runs, prints the PASS line.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] fault_cross_module: program errored (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

if ! printf '%s\n' "$out" | grep -q "PASS: fault_cross_module"; then
    echo "  [FAIL] fault_cross_module: missing PASS line"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] fault_cross_module: cross-module fault identity, no same-name collision"
exit 0
