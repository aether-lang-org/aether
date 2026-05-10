#!/bin/sh
# Integration test for multi-entry --lib search path (issue #413).
# Five cases — each covers one of the documented surfaces:
#
#   1. Single-entry --lib (today's behaviour, preserved)
#   2. Multi-entry --lib via PATH-style separator (`a:b` POSIX / `a;b` Win)
#   3. Multi-entry --lib via repeated flag (`--lib a --lib b`)
#   4. AETHER_LIB_DIR env var with the same separator-string shape
#   5. Precedence: left-most wins on module-name collisions
#
# All five fixtures live next to this script under dirA/ and dirB/:
#   dirA/foo    foo_origin() -> "A"
#   dirB/foo    foo_origin() -> "B"
#   dirB/bar    bar_origin() -> "bar-from-B"
# The main.ae driver imports both `foo` and `bar` and prints
# `foo=<x> bar=<y>` so each case asserts which directories the
# resolution walked.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

# Platform path separator (matches AETHER_LIB_PATH_SEP_CHAR in tools/ae.c).
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) SEP=";";;
    *)                                 SEP=":";;
esac

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

pass=0; fail=0

run_case() {
    label="$1"; expected="$2"; shift 2
    out=$("$@" 2>&1)
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  [FAIL] $label: ae exited $rc"
        echo "    output: $out"
        fail=$((fail + 1)); return
    fi
    last=$(printf '%s\n' "$out" | tail -1)
    if [ "$last" = "$expected" ]; then
        echo "  [PASS] $label"
        pass=$((pass + 1))
    else
        echo "  [FAIL] $label"
        echo "    expected: $expected"
        echo "    got:      $last"
        fail=$((fail + 1))
    fi
}

# Unset AETHER_LIB_DIR so it doesn't bleed in from the test harness
# environment.
unset AETHER_LIB_DIR

# Case 1: single-entry --lib (only dirA exists in this case — we
# expect the `bar` import to fail, so the test imports only `foo`).
# Use a small driver that just imports foo to keep the case clean.
mkdir -p "$TMPDIR/only_foo"
cat > "$TMPDIR/only_foo/main.ae" <<EOF
import foo
main() { println("foo=\${foo.foo_origin()}") }
EOF
run_case "single-entry --lib (today's shape)" \
    "foo=A" \
    "$AE" run "$TMPDIR/only_foo/main.ae" --lib "$SCRIPT_DIR/dirA"

# Case 2: multi-entry separator-string. dirA first (foo from A),
# dirB second (bar from B).
run_case "multi-entry --lib via separator-string" \
    "foo=A bar=bar-from-B" \
    "$AE" run "$SCRIPT_DIR/main.ae" --lib "$SCRIPT_DIR/dirA${SEP}$SCRIPT_DIR/dirB"

# Case 3: repeated --lib flags compose. Same outcome as case 2.
run_case "multi-entry via repeated --lib flag" \
    "foo=A bar=bar-from-B" \
    "$AE" run "$SCRIPT_DIR/main.ae" --lib "$SCRIPT_DIR/dirA" --lib "$SCRIPT_DIR/dirB"

# Case 4: AETHER_LIB_DIR env var with the same separator shape.
run_case "AETHER_LIB_DIR env var (separator-string)" \
    "foo=A bar=bar-from-B" \
    env AETHER_LIB_DIR="$SCRIPT_DIR/dirA${SEP}$SCRIPT_DIR/dirB" "$AE" run "$SCRIPT_DIR/main.ae"

# Case 5: precedence — flip the order, dirB first. foo now resolves
# to B (B's foo wins via left-most-first), bar still from B.
run_case "left-most wins on collision" \
    "foo=B bar=bar-from-B" \
    "$AE" run "$SCRIPT_DIR/main.ae" --lib "$SCRIPT_DIR/dirB${SEP}$SCRIPT_DIR/dirA"

echo
echo "run_lib_path: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
