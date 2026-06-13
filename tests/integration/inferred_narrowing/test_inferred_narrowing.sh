#!/bin/sh
# Regression (#698): a local whose type was INFERRED from a 32-bit int
# initializer, later re-bound with a 64-bit value, truncates silently —
# the aedis lpStringEqualsInt64 corruption. The compiler now diagnoses
# that narrowing, but leaves explicitly-annotated declarations alone
# (the annotation is the user's deliberate choice).
#
# Acceptance:
#   bad_narrow.ae      -> compile error naming the narrowing + the fix
#   ok_long.ae         -> builds clean (keeps 64-bit), runs, prints the value
#   ok_explicit_int.ae -> builds clean (deliberate int narrowing), no error

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
AE="$ROOT/build/ae"

fail=0
tmpdir="$(mktemp -d)"

# (1) inferred narrowing -> error that names the variable + the fix.
log="$tmpdir/bad.log"
"$AETHERC" "$SCRIPT_DIR/bad_narrow.ae" "$tmpdir/bad.c" >"$log" 2>&1
if ! grep -qi "narrowing assignment to 'parsed'" "$log"; then
    echo "  [FAIL] inferred_narrowing: expected a narrowing error for the inferred local"
    sed 's/^/        /' "$log" | head -6
    fail=1
fi
if ! grep -qi "long parsed" "$log"; then
    echo "  [FAIL] inferred_narrowing: error should suggest the \`long\` annotation"
    fail=1
fi

# (2) explicit long -> builds clean (the guard must not fire) and keeps
#     64-bit precision. Build success == no narrowing error.
if ! "$AE" build "$SCRIPT_DIR/ok_long.ae" -o "$tmpdir/ok_long" >/dev/null 2>&1 \
   || [ ! -x "$tmpdir/ok_long" ]; then
    echo "  [FAIL] inferred_narrowing: explicit \`long\` should build clean (guard must not fire)"
    fail=1
elif [ "$("$tmpdir/ok_long" 2>/dev/null)" != "6288338901" ]; then
    echo "  [FAIL] inferred_narrowing: explicit long lost precision"
    fail=1
fi

# (3) explicit int -> deliberate narrowing, builds clean (no error).
if ! "$AE" build "$SCRIPT_DIR/ok_explicit_int.ae" -o "$tmpdir/ok_int" >/dev/null 2>&1 \
   || [ ! -x "$tmpdir/ok_int" ]; then
    echo "  [FAIL] inferred_narrowing: explicit \`int\` (deliberate) should build clean"
    fail=1
fi

rm -rf "$tmpdir"
if [ "$fail" -eq 0 ]; then
    echo "  [PASS] inferred_narrowing: diagnoses inferred narrowing, allows explicit annotations"
fi
exit $fail
