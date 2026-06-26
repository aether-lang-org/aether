#!/bin/sh
# Issue #878: a module's qualified `X.fn()` surface is available whenever the
# namespace is imported in ANY form (bare, selective, or qualified) — like
# Java's always-legal fully-qualified name. A selective import
# (`import std.math (sqrt)`) is purely additive: it adds the bare-name binding
# (`sqrt(...)`) ON TOP OF the always-available qualified surface
# (`math.sqrt(...)`, `math.pow(...)`). It no longer restricts the qualified
# form. (This replaces the old behavior, where `math.pow` was rejected under a
# selective `import std.math (sqrt)`.)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

pass=0
fail=0

check_ok() { # desc, file
    if AETHER_HOME="" "$ROOT/build/ae" check "$2" 2>/dev/null; then
        echo "  [PASS] $1"; pass=$((pass + 1))
    else
        echo "  [FAIL] $1"; fail=$((fail + 1))
    fi
}

# 1. Qualified call to a NON-selected function under a selective import — now
#    resolves (#878). Previously this was rejected.
cat > /tmp/ae_sel_qual_pow.ae << 'EOF'
import std.math (sqrt)
main() {
    y = math.pow(2.0, 3.0)
    println(y)
}
EOF
check_ok "qualified math.pow resolves under 'import std.math (sqrt)'" /tmp/ae_sel_qual_pow.ae

# 2. Bare-name binding the selective import adds still works.
cat > /tmp/ae_sel_bare.ae << 'EOF'
import std.math (sqrt)
main() {
    x = sqrt(9.0)
    println(x)
}
EOF
check_ok "bare-name sqrt() works under selective import" /tmp/ae_sel_bare.ae

# 3. Qualified call to the SELECTED function also works.
cat > /tmp/ae_sel_qual_sqrt.ae << 'EOF'
import std.math (sqrt)
main() {
    x = math.sqrt(9.0)
    println(x)
}
EOF
check_ok "qualified math.sqrt works under selective import" /tmp/ae_sel_qual_sqrt.ae

# 4. Full (bare) import still exposes the whole qualified surface.
cat > /tmp/ae_sel_full.ae << 'EOF'
import std.math
main() {
    println(math.sqrt(9.0))
    println(math.pow(2.0, 3.0))
}
EOF
check_ok "bare import exposes the full qualified surface" /tmp/ae_sel_full.ae

rm -f /tmp/ae_sel_qual_pow.ae /tmp/ae_sel_bare.ae /tmp/ae_sel_qual_sqrt.ae /tmp/ae_sel_full.ae

echo ""
echo "Selective-import qualified-surface tests: $pass passed, $fail failed"
if [ "$fail" -gt 0 ]; then exit 1; fi
