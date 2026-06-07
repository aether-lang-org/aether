#!/bin/sh
# contrib.templating.liquid — `{% extends %}` alias + `{{ block.super }}`.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
PARTIAL_ROOT="$SCRIPT_DIR/partials"

OUT="$(AETHER_HOME="$ROOT" LIQUID_PARTIAL_ROOT="$PARTIAL_ROOT" "$AE" run "$SCRIPT_DIR/probe.ae" 2>&1)"
RC=$?
if [ $RC -ne 0 ]; then
    echo "  [FAIL] liquid_extends_super (exit $RC):"
    echo "$OUT" | sed 's/^/    /'
    exit 1
fi
if ! echo "$OUT" | grep -q "All PASS"; then
    echo "  [FAIL] liquid_extends_super — no 'All PASS' line:"
    echo "$OUT" | sed 's/^/    /'
    exit 1
fi

echo "  [PASS] liquid_extends_super: 10/10"
