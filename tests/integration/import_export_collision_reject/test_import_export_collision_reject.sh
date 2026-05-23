#!/bin/sh
# Tests that a user top-level function whose name forges the C symbol an
# imported module export mangles to (`proxy_opts_new` vs `proxy.opts_new`,
# both → flat C symbol `proxy_opts_new`) is rejected at compile time with
# error[E1001] — rather than the merge silently skipping the import clone
# and binding every `proxy.opts_new(...)` call to the user's function
# (clean compile, clean link, wrong function at runtime).
#
# See user-identifiers-must-not-collide-java-style-scoping.md.
#
# Also asserts the non-collisions still compile:
#   - the same local name WITHOUT importing the colliding module
#   - importing the module with a local name that does NOT collide

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

pass=0
fail=0

# --- Case 1: local `proxy_opts_new` + import std.http.proxy → reject ----
cat > /tmp/ae_iec_bad.ae << 'EOF'
import std.http.proxy
extern println(s: string)

proxy_opts_new() -> ptr { return 0 }

main() {
    o1 = proxy.opts_new()
    println("done")
}
EOF
output=$(AETHER_HOME="$ROOT" "$AE" build /tmp/ae_iec_bad.ae -o /tmp/ae_iec_bad_out 2>&1)
if echo "$output" | grep -q "error\[E1001\]"; then
    echo "  [PASS] import/export symbol collision rejected with error[E1001]"
    pass=$((pass + 1))
else
    echo "  [FAIL] expected error[E1001] collision diagnostic; got:"
    echo "$output" | head -10 | sed 's/^/    /'
    fail=$((fail + 1))
fi

# --- Case 2: same local name, NO colliding import → compiles ------------
cat > /tmp/ae_iec_noimport.ae << 'EOF'
extern println(s: string)

proxy_opts_new() -> ptr { return 0 }

main() {
    _p = proxy_opts_new()
    println("ok")
}
EOF
if AETHER_HOME="$ROOT" "$AE" build /tmp/ae_iec_noimport.ae -o /tmp/ae_iec_noimport_out >/tmp/ae_iec_ni.log 2>&1; then
    echo "  [PASS] same name without the colliding import still compiles"
    pass=$((pass + 1))
else
    echo "  [FAIL] false positive: no import should mean no collision:"
    head -10 /tmp/ae_iec_ni.log | sed 's/^/    /'
    fail=$((fail + 1))
fi

# --- Case 3: import the module, non-colliding local name → compiles -----
cat > /tmp/ae_iec_ok.ae << 'EOF'
import std.http.proxy
extern println(s: string)

px_opts_helper() -> ptr { return 0 }

main() {
    _p = px_opts_helper()
    println("ok")
}
EOF
if AETHER_HOME="$ROOT" "$AE" build /tmp/ae_iec_ok.ae -o /tmp/ae_iec_ok_out >/tmp/ae_iec_ok.log 2>&1; then
    echo "  [PASS] non-colliding local name with the import compiles"
    pass=$((pass + 1))
else
    echo "  [FAIL] false positive: non-colliding name rejected:"
    head -10 /tmp/ae_iec_ok.log | sed 's/^/    /'
    fail=$((fail + 1))
fi

rm -f /tmp/ae_iec_*.ae /tmp/ae_iec_*_out /tmp/ae_iec_*.log
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
