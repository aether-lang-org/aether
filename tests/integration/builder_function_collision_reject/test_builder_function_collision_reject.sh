#!/bin/sh
# Tests that a `builder` and a plain function sharing a name are
# rejected at typecheck time with a clear "duplicate definition"
# diagnostic — rather than silently emitting one C symbol and
# dispatching every call to whichever definition the linker resolves
# first (clean compile, clean link, wrong function body, exit 0).
#
# See docs/notes/builder-function-name-collision-silent-dispatch.md.
#
# Also asserts the two NON-collisions still compile:
#   - a plain multi-clause function (same name, pattern-matching form)
#   - a builder and function with DISTINCT names

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

pass=0
fail=0

# --- Case 1: builder + function same name → rejected ---------------
cat > /tmp/ae_bfc_bad.ae << 'EOF'
extern println(s: string)

gem(ctx: ptr, line: string) {
    println("SETTER")
}

builder gem(ctx: ptr) {
    println("BUILDER")
}

main() {
    println("x")
}
EOF
output=$(AETHER_HOME="" "$ROOT/build/ae" build /tmp/ae_bfc_bad.ae -o /tmp/ae_bfc_bad_out 2>&1)
if echo "$output" | grep -q "duplicate definition of 'gem'"; then
    echo "  [PASS] builder+function name collision rejected with diagnostic"
    pass=$((pass + 1))
else
    echo "  [FAIL] expected duplicate-definition diagnostic; got:"
    echo "$output" | head -10 | sed 's/^/    /'
    fail=$((fail + 1))
fi

# --- Case 2: multi-clause plain function (same name) → OK ----------
# Two clauses of the same plain function is the legal pattern-matching
# form and must NOT be flagged as a collision.
cat > /tmp/ae_bfc_multiclause.ae << 'EOF'
extern exit(code: int)

classify(0) -> int { return 100 }
classify(n: int) -> int { return n }

main() {
    if classify(0) != 100 { exit(1) }
    if classify(7) != 7 { exit(2) }
    exit(0)
}
EOF
if AETHER_HOME="" "$ROOT/build/ae" build /tmp/ae_bfc_multiclause.ae -o /tmp/ae_bfc_mc_out 2>/dev/null; then
    if /tmp/ae_bfc_mc_out; then
        echo "  [PASS] multi-clause plain function not flagged as collision"
        pass=$((pass + 1))
    else
        echo "  [FAIL] multi-clause function compiled but ran wrong"
        fail=$((fail + 1))
    fi
else
    echo "  [FAIL] multi-clause plain function should still compile"
    fail=$((fail + 1))
fi

# --- Case 3: builder + function with DISTINCT names → OK -----------
cat > /tmp/ae_bfc_distinct.ae << 'EOF'
extern exit(code: int)
extern println(s: string)

gem_set(ctx: ptr, line: string) {
    println("SETTER")
}

builder gem(ctx: ptr) {
    println("BUILDER")
}

main() {
    exit(0)
}
EOF
if AETHER_HOME="" "$ROOT/build/ae" build /tmp/ae_bfc_distinct.ae -o /tmp/ae_bfc_d_out 2>/dev/null; then
    echo "  [PASS] builder + function with distinct names compiles"
    pass=$((pass + 1))
else
    echo "  [FAIL] distinct-named builder + function should compile"
    fail=$((fail + 1))
fi

# Cleanup
rm -f /tmp/ae_bfc_bad.ae /tmp/ae_bfc_multiclause.ae /tmp/ae_bfc_distinct.ae \
      /tmp/ae_bfc_bad_out /tmp/ae_bfc_mc_out /tmp/ae_bfc_d_out

echo ""
echo "builder_function_collision_reject: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
