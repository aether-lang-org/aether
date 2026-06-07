#!/bin/sh
# contrib.templating.liquid — Phase-1 typed value model (scalars).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

OUT="$(AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/probe.ae" 2>&1)"
RC=$?
if [ $RC -ne 0 ]; then
    echo "  [FAIL] liquid_value_basics (exit $RC):"
    echo "$OUT" | sed 's/^/    /'
    exit 1
fi
if ! echo "$OUT" | grep -q "All PASS"; then
    echo "  [FAIL] liquid_value_basics — no 'All PASS' line:"
    echo "$OUT" | sed 's/^/    /'
    exit 1
fi

echo "  [PASS] liquid_value_basics: 13/13"
