#!/bin/sh
# Issue #1140: `defer try` / `defer catch` only mean something in a function that
# can actually fail. In a function with no error channel a `defer catch` can never
# fire, and a `defer try` is just a plain `defer` — either way the code does not do
# what it says, so the compiler must WARN rather than silently accept it.
#
# These are warnings, not errors: the programs still compile and run. So this test
# asserts on the diagnostic text, and — just as importantly — that the two VALID
# shapes stay completely silent. A warning that cries wolf on correct code is worse
# than no warning at all.
#
# Pruned from the generic .ae runner (these sources exist to be compiled and have
# their diagnostics inspected, not to be run as tests).
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
if [ ! -x "$AETHERC" ]; then echo "  [SKIP] defer_conditional_warn: $AETHERC not built"; exit 0; fi
TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

# $1 = source basename, $2 = extended regex the warning must match
expect_warning() {
    name="$1"
    want="$2"
    "$AETHERC" "$SCRIPT_DIR/$name.ae" "$TMPDIR/$name.c" >"$TMPDIR/$name.log" 2>&1
    if ! grep -qiE "$want" "$TMPDIR/$name.log"; then
        echo "  [FAIL] defer_conditional_warn: $name.ae did not warn as expected"
        echo "         expected a diagnostic matching: $want"
        sed 's/^/    /' "$TMPDIR/$name.log" | head -8
        exit 1
    fi
}

# The valid shapes must be SILENT — no warning at all.
expect_no_warning() {
    name="$1"
    "$AETHERC" "$SCRIPT_DIR/$name.ae" "$TMPDIR/$name.c" >"$TMPDIR/$name.log" 2>&1
    if grep -qi "warning" "$TMPDIR/$name.log"; then
        echo "  [FAIL] defer_conditional_warn: $name.ae warned, but it is valid code"
        sed 's/^/    /' "$TMPDIR/$name.log" | head -8
        exit 1
    fi
}

expect_warning warn_catch_infallible 'defer catch.*cannot fail.*never run'
expect_warning warn_try_infallible   'defer try.*cannot fail.*same as a plain'
expect_no_warning ok_value_err
expect_no_warning ok_result

echo "  [PASS] defer_conditional_warn: infallible-function misuse warns; (value, err) and T! stay silent"
exit 0
