#!/bin/sh
# Issue #925: the circular-import diagnostic must name the ACTUAL modules in
# the cycle, in order — not the bogus `involving module '__main__'` at 0:0.
#
# Fixture: lib/a imports b, lib/b imports a (a -> b -> a); main imports a.
# Acceptance:
#   1. an error IS reported,
#   2. it names the cycle as `a -> b -> a` (participating modules, in order),
#   3. it does NOT say `__main__` (the synthetic entry root is not a cycle
#      member and must not be blamed).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

tmpdir="$(mktemp -d)"
log="$tmpdir/cc.log"
fail=0

# Build from the fixture dir so `import a` resolves against lib/.
( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" build main.ae -o "$tmpdir/out" ) >"$log" 2>&1

if ! grep -qi "circular import" "$log"; then
    echo "  [FAIL] circular_import_diag: no circular-import error reported"
    sed 's/^/          /' "$log" | head -10
    fail=1
fi

# (2) names the actual cycle, modules in order
if ! grep -q 'a -> b -> a' "$log"; then
    echo "  [FAIL] circular_import_diag: diagnostic does not name the cycle 'a -> b -> a'"
    grep -i circular "$log" | sed 's/^/          /' | head -4
    fail=1
fi

# (3) must NOT blame the synthetic __main__ root
if grep -q '__main__' "$log"; then
    echo "  [FAIL] circular_import_diag: diagnostic still blames '__main__'"
    grep -i circular "$log" | sed 's/^/          /' | head -4
    fail=1
fi

rm -rf "$tmpdir"
if [ "$fail" -eq 0 ]; then
    echo "  [PASS] circular_import_diag: cycle named as 'a -> b -> a', not __main__"
fi
exit $fail
