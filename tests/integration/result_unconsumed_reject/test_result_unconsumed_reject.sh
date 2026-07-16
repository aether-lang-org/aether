#!/bin/sh
# Phase 2 of error-unification: a `T!` result used as a bare expression
# statement is a compile error — the fallible outcome must be consumed
# (destructured, `!`-propagated, `or`-handled, or explicitly discarded with
# `_, _ = ...`). Enforcement keys on `is_result`, so a raw `(value, string)`
# tuple that has NOT migrated to `T!` is deliberately still droppable.
# The bare-drop fixtures MUST fail to build with the "must be consumed"
# diagnostic; the raw-tuple control MUST build. Pruned from the generic .ae
# runner (these are compile-outcome fixtures, not runnable success tests).
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
if [ ! -x "$AETHERC" ]; then echo "  [SKIP] result_unconsumed_reject: $AETHERC not built"; exit 0; fi
TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

check_rejected() {
    name="$1"
    if "$AETHERC" "$SCRIPT_DIR/$name.ae" "$TMPDIR/$name.c" >"$TMPDIR/$name.log" 2>&1; then
        echo "  [FAIL] result_unconsumed_reject: $name.ae compiled (should be rejected)"; exit 1
    fi
    if ! grep -qi "must be consumed" "$TMPDIR/$name.log"; then
        echo "  [FAIL] result_unconsumed_reject: $name.ae rejected, but not for the unconsumed-result reason"
        sed 's/^/    /' "$TMPDIR/$name.log" | head -6; exit 1
    fi
}

check_accepted() {
    name="$1"
    if ! "$AETHERC" "$SCRIPT_DIR/$name.ae" "$TMPDIR/$name.c" >"$TMPDIR/$name.log" 2>&1; then
        echo "  [FAIL] result_unconsumed_reject: $name.ae should COMPILE (raw tuple, not a T!)"
        sed 's/^/    /' "$TMPDIR/$name.log" | head -6; exit 1
    fi
}

check_rejected reject_bare_call
check_rejected reject_bare_user_fn
check_accepted reject_raw_tuple_not_flagged

echo "  [PASS] result_unconsumed_reject: bare T! drop rejected; raw tuple still droppable"
exit 0
