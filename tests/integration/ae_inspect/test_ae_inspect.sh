#!/bin/sh
# Regression (#473): `ae inspect <file.ae>` prints an operator-facing
# summary of what the script declares — artifact shape + entry point,
# capability posture from gated imports, the resolved import list,
# top-level declarations (functions with signatures, structs, consts).
#
# Asserts the salient lines for the fixture. Scoping matters: the report
# must show only what THIS file declares, not the std.fs internals merged
# in during import resolution (so e.g. fs_KIND_* constants must NOT leak).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

fail=0
out="$("$AE" inspect "$SCRIPT_DIR/svc.ae" 2>&1)"

assert_has() {
    if ! printf '%s\n' "$out" | grep -qF "$1"; then
        echo "  [FAIL] ae_inspect: expected output to contain: $1"
        fail=1
    fi
}
assert_lacks() {
    if printf '%s\n' "$out" | grep -qF "$1"; then
        echo "  [FAIL] ae_inspect: output should NOT contain: $1"
        fail=1
    fi
}

assert_has "artifact:  executable (entry: main)"
assert_has "capabilities required (gated imports): fs"
assert_has "std.fs"
assert_has "capability: fs"
assert_has "greet(name: string) -> string"
assert_has "structs (1): Config"
assert_has "DEFAULT_PORT"
# Imported-module internals must not bleed into the user's view.
assert_lacks "fs_KIND_"

if [ "$fail" -ne 0 ]; then
    echo "  ---- ae inspect output was: ----"
    printf '%s\n' "$out" | sed 's/^/  | /'
    exit 1
fi

echo "  [PASS] ae_inspect: declaration summary for svc.ae"
exit 0
