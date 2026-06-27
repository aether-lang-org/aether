#!/bin/sh
# Issue #934: UFCS (#928) must resolve across the import boundary. A
# `value.method()` call should find a `method` exported by an imported
# module whose first parameter matches typeof(value) — honoring the same
# visibility as a normal qualified `mod.method(value)` call. Pre-fix, UFCS
# only searched same-file functions, so library-provided fluent surfaces
# (the whole point — a test framework's expect(x).to_equal(5)) could not be
# chained from a consumer file.
#
# Fixture (lib/assert.ae): a fluent assertion facade exporting expect_int,
# to_equal, to_be_gt, bump on a Subject struct. main.ae imports it and:
#   - calls a UFCS method on a STORED imported value (a.bump())   -> 8
#   - chains UFCS across the boundary on a CALL RESULT receiver   -> ok 1
#   - a failing chain still resolves and reports the failure      -> ok 0
#
# Acceptance: compiles, links, runs, prints exactly "8", "1", "0".

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] ufcs_cross_module: program errored (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$got" != "8 1 0" ]; then
    echo "  [FAIL] ufcs_cross_module: expected '8 1 0', got '$got'"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] ufcs_cross_module: UFCS resolves imported methods (stored + chained receivers)"
exit 0
