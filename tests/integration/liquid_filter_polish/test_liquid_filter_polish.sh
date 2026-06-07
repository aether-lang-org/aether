#!/bin/sh
# contrib.templating.liquid — filter polish: truncate custom ellipsis
# + default with allow_false.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

OUT="$(AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/probe.ae" 2>&1)"
RC=$?
if [ $RC -ne 0 ]; then
    echo "  [FAIL] liquid_filter_polish (exit $RC):"
    echo "$OUT" | sed 's/^/    /'
    exit 1
fi
if ! echo "$OUT" | grep -q "All PASS"; then
    echo "  [FAIL] liquid_filter_polish — no 'All PASS' line:"
    echo "$OUT" | sed 's/^/    /'
    exit 1
fi

echo "  [PASS] liquid_filter_polish: 15/15"
