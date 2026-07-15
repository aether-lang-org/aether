#!/bin/sh
# Nested optionals `T??` are rejected at parse time: a double optional parses
# as ae_opt_ae_opt_<T> but the rest of the compiler reasons only one presence
# layer deep (none/wrap coercion, == none, narrowing), so it would miscompile
# silently. Matches C3, whose type_add_optional refuses to nest. Each source
# MUST fail to build with the nested-optional diagnostic. Pruned from the
# generic .ae runner.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
if [ ! -x "$AETHERC" ]; then echo "  [SKIP] optional_reject: $AETHERC not built"; exit 0; fi
TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

check_rejected() {
    name="$1"
    if "$AETHERC" "$SCRIPT_DIR/$name.ae" "$TMPDIR/$name.c" >"$TMPDIR/$name.log" 2>&1; then
        echo "  [FAIL] optional_reject: $name.ae compiled (should be rejected)"; exit 1
    fi
    if ! grep -qi "nested optional" "$TMPDIR/$name.log"; then
        echo "  [FAIL] optional_reject: $name.ae rejected, but not for the nested-optional reason"
        sed 's/^/    /' "$TMPDIR/$name.log" | head -6; exit 1
    fi
}

check_rejected reject_return_nested
check_rejected reject_let_nested
check_rejected reject_param_nested

echo "  [PASS] optional_reject: T?? rejected in return / let / param position"
exit 0
