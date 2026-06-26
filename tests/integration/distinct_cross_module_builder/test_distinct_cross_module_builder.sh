#!/bin/sh
# Regression #908: a `type X = distinct Base` imported from another module must
# resolve in cross-module constructors/unwraps AND in builder-child (`_ctx`)
# functions. Before the fix the imported distinct def wasn't merged into the
# consumer program, so `value as int` failed ("cannot cast Num to int") and
# codegen emitted an unknown C type `Num`.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] ae not built"; exit 0; }
OUT="$(cd "$SCRIPT_DIR" && AETHER_HOME="$ROOT" "$AE" run --lib ./lib main.ae 2>&1)"
EXPECT="5
7"
if [ "$OUT" = "$EXPECT" ]; then
    echo "  [PASS] distinct_cross_module_builder: imported distinct resolves in builder child + cross-module"
else
    echo "  [FAIL] distinct_cross_module_builder:"; echo "$OUT" | sed 's/^/      /'; exit 1
fi
