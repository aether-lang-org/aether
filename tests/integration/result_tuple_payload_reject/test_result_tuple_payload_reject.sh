#!/bin/sh
# A tuple payload in `T!` — `(A, B)!` — is rejected at parse time. It would
# lower to the nested tuple `((A, B), string)`, which is not ABI-compatible
# with the flat `(A, B, string)` tuple the stdlib multi-value fallibles use,
# and a 3-way `a, b, e = f()` destructure sees only 2 values and fails with a
# confusing count mismatch downstream. `T!` is the single-payload fallible
# spine; a multi-value fallible stays a raw `(A, B, string)` tuple. Each
# source MUST fail to build with the tuple-payload diagnostic. Pruned from the
# generic .ae runner (these are compile-error fixtures, not runnable).
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
if [ ! -x "$AETHERC" ]; then echo "  [SKIP] result_tuple_payload_reject: $AETHERC not built"; exit 0; fi
TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

check_rejected() {
    name="$1"
    if "$AETHERC" "$SCRIPT_DIR/$name.ae" "$TMPDIR/$name.c" >"$TMPDIR/$name.log" 2>&1; then
        echo "  [FAIL] result_tuple_payload_reject: $name.ae compiled (should be rejected)"; exit 1
    fi
    if ! grep -qi "tuple payload in .T!." "$TMPDIR/$name.log"; then
        echo "  [FAIL] result_tuple_payload_reject: $name.ae rejected, but not for the tuple-payload reason"
        sed 's/^/    /' "$TMPDIR/$name.log" | head -6; exit 1
    fi
}

check_rejected reject_return_tuple
check_rejected reject_param_tuple
check_rejected reject_let_tuple

echo "  [PASS] result_tuple_payload_reject: (A, B)! rejected in return / param / let position"
exit 0
