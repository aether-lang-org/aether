#!/bin/sh
# The value-producing `or { }` form: a block handler must end with a value
# (assignable to the success type) or an exit statement. Every other ending
# used to compile and read an UNINITIALIZED result on the error path; now it
# is rejected at typecheck. Each reject_*.ae MUST fail to build for the
# stated reason. Pruned from the generic .ae runner.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
if [ ! -x "$AETHERC" ]; then echo "  [SKIP] or_block_reject: $AETHERC not built"; exit 0; fi
TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

check_rejected() {
    name="$1"; want="$2"
    if "$AETHERC" "$SCRIPT_DIR/$name.ae" "$TMPDIR/$name.c" >"$TMPDIR/$name.log" 2>&1; then
        echo "  [FAIL] or_block_reject: $name.ae compiled (should be rejected)"; exit 1
    fi
    if ! grep -qiE "$want" "$TMPDIR/$name.log"; then
        echo "  [FAIL] or_block_reject: $name.ae rejected, but not for the stated reason"
        echo "         expected a diagnostic matching: $want"
        sed 's/^/    /' "$TMPDIR/$name.log" | head -6; exit 1
    fi
}

check_rejected reject_trailing_if 'handler must end with a value'
check_rejected reject_empty_block 'handler must end with a value'
check_rejected reject_wrong_type  'handler yields string where the expression.s value type is int'

echo "  [PASS] or_block_reject: trailing-if / empty / wrong-type handler blocks all rejected"
exit 0
