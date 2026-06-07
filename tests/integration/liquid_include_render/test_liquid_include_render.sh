#!/bin/sh
# contrib.templating.liquid — `{% include %}` and `{% render %}` tags.
#
# Passes the absolute path to the test's `partials/` directory through
# `LIQUID_PARTIAL_ROOT`, which the probe reads via `os.getenv`.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
PARTIAL_ROOT="$SCRIPT_DIR/partials"

OUT="$(AETHER_HOME="$ROOT" LIQUID_PARTIAL_ROOT="$PARTIAL_ROOT" "$AE" run "$SCRIPT_DIR/probe.ae" 2>&1)"
RC=$?
if [ $RC -ne 0 ]; then
    echo "  [FAIL] liquid_include_render (exit $RC):"
    echo "$OUT" | sed 's/^/    /'
    exit 1
fi
if ! echo "$OUT" | grep -q "All PASS"; then
    echo "  [FAIL] liquid_include_render — no 'All PASS' line:"
    echo "$OUT" | sed 's/^/    /'
    exit 1
fi

echo "  [PASS] liquid_include_render: 17/17"
