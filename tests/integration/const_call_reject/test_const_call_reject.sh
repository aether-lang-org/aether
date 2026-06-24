#!/bin/sh
# Tests that `const X = some_function_call()` is rejected at
# typecheck time. Per Nico's design call: const inlines its RHS at
# every use site, so `const G = make_thing()` would silently re-call
# the function on every reference. Reject these at compile time with
# a helpful diagnostic that points the user at std.config /
# std.actors for process-global state. Section 2 of
# nuther-ask-of-aether-team.md.
#
# Exception (issue #482, compile-time constant evaluation phase-1):
# a HARD-WHITELISTED set of pure conversions — string.from_int /
# string.from_long / string.from_float / string.concat — IS admissible
# in a const initializer because the optimizer folds them to a literal
# at compile time (no re-evaluation at use sites). Every OTHER call
# stays rejected. See docs/compile-time-eval.md.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

pass=0
fail=0

expect_reject() {
    label="$1"
    file="$2"
    out=$(AETHER_HOME="" "$ROOT/build/ae" build "$file" -o /tmp/ae_const_reject_out 2>&1)
    if echo "$out" | grep -q "const initializer must be a compile-time constant expression"; then
        echo "  [PASS] $label"
        pass=$((pass + 1))
    else
        echo "  [FAIL] $label — expected reject; got:"
        echo "$out" | head -8 | sed 's/^/    /'
        fail=$((fail + 1))
    fi
}

expect_accept() {
    label="$1"
    file="$2"
    if AETHER_HOME="" "$ROOT/build/ae" build "$file" -o /tmp/ae_const_ok_out 2>/dev/null; then
        echo "  [PASS] $label"
        pass=$((pass + 1))
    else
        echo "  [FAIL] $label — expected accept"
        fail=$((fail + 1))
    fi
}

# Case 1: top-level const = function call (the headline trap).
cat > /tmp/ae_const_bad1.ae << 'EOF'
extern malloc(size: int) -> ptr
const G_BUF = malloc(64)
main() { println("ok") }
EOF
expect_reject "top-level const = function call" /tmp/ae_const_bad1.ae

# Case 2: top-level const = NON-whitelisted std-namespaced call.
# string.length is a real pure stdlib function but it is NOT on the
# compile-time-fold whitelist, so it must still be rejected.
cat > /tmp/ae_const_bad2.ae << 'EOF'
import std.string
const N = string.length("hi")
main() { println("ok") }
EOF
expect_reject "top-level const = non-whitelisted std call" /tmp/ae_const_bad2.ae

# Case 2b (issue #482): the whitelisted pure conversions ARE accepted
# and fold to a literal — string.from_int / from_long / from_float /
# concat. These do not re-evaluate at use sites because the optimizer
# replaces the call with the literal it produces.
cat > /tmp/ae_const_ok_fold.ae << 'EOF'
import std.string
const ID = string.from_int(42)
const VER = string.concat("v", "1")
main() { println("ok") }
EOF
expect_accept "whitelisted from_int/concat const folds (issue #482)" /tmp/ae_const_ok_fold.ae

# Case 3: function-local const = call.
cat > /tmp/ae_const_bad3.ae << 'EOF'
extern malloc(size: int) -> ptr
main() {
    const LOCAL = malloc(64)
    println("ok")
}
EOF
expect_reject "function-local const = call" /tmp/ae_const_bad3.ae

# Case 4: legit literal const compiles.
cat > /tmp/ae_const_ok1.ae << 'EOF'
const PI = 3.14
const MAX = 100
const NAME = "alice"
main() { println("ok") }
EOF
expect_accept "literal const RHS still compiles" /tmp/ae_const_ok1.ae

# Case 5: arithmetic on literals compiles.
cat > /tmp/ae_const_ok2.ae << 'EOF'
const MAX = 100
const HALF = MAX / 2
const DOUBLE = MAX * 2
main() { println("ok") }
EOF
expect_accept "arithmetic on const RHS still compiles" /tmp/ae_const_ok2.ae

rm -f /tmp/ae_const_bad*.ae /tmp/ae_const_ok*.ae \
      /tmp/ae_const_reject_out /tmp/ae_const_ok_out /tmp/ae_const_ok_fold.ae

echo ""
echo "const_call_reject: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
