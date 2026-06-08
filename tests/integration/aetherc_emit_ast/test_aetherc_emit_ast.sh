#!/bin/sh
# Verifies aetherc --emit=ast emits a stable, parseable JSON
# representation of the post-typecheck program AST for aeb's
# supply-chain veto. See veto-enhancements.md / .reply.md /
# .reply2.md for the cross-repo design.
#
# Cells:
#   1.  Qualified call (`import std.os` + `os.system(...)`) — callee
#       canonicalises to `os_system`. Defeats the regex case.
#   2.  Selective import (`import std.os (system)` + bare `system(...)`)
#       — selected[] list present, callee canonicalises via the merger.
#   3.  Bare `extern` declaration — surfaced as
#       extern_function with name + variadic flag.
#   4.  Variadic extern — variadic:true flag present.
#   5.  Glob import (`import std.os (*)`) — glob:true flag present.
#   6.  Per-node file + line populated.
#   7.  Parse failure → non-zero exit (aeb fail-closes on this).
#   8.  Typecheck failure → non-zero exit (ditto).
#   9.  Empty file → exit 0, empty nodes array.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] aetherc_emit_ast on Windows"; exit 0 ;;
esac

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
pass=0; fail=0

assert_contains() {
    label="$1"; needle="$2"; haystack="$3"
    if echo "$haystack" | grep -qF "$needle"; then
        echo "  [PASS] $label"
        pass=$((pass + 1))
    else
        echo "  [FAIL] $label — '$needle' not in output:"
        echo "$haystack" | sed 's/^/      /'
        fail=$((fail + 1))
    fi
}

assert_not_contains() {
    label="$1"; needle="$2"; haystack="$3"
    if echo "$haystack" | grep -qF "$needle"; then
        echo "  [FAIL] $label — '$needle' unexpectedly present:"
        echo "$haystack" | sed 's/^/      /'
        fail=$((fail + 1))
    else
        echo "  [PASS] $label"
        pass=$((pass + 1))
    fi
}

# Cell 1: qualified call canonicalisation.
cat > "$TMPDIR/qualified.ae" <<'EOF'
import std.os

main() {
    rc = os.system("echo hi")
}
EOF
out1="$(AETHER_HOME="$ROOT" "$AETHERC" --emit=ast "$TMPDIR/qualified.ae" 2>/dev/null)"
rc1=$?
[ "$rc1" -eq 0 ] && { echo "  [PASS] 1a exit 0"; pass=$((pass+1)); } || { echo "  [FAIL] 1a exit nonzero"; fail=$((fail+1)); }
assert_contains "1b callee canonicalises to os_system" '"callee":"os_system"' "$out1"
assert_contains "1c module is std.os" '"module":"std.os"' "$out1"

# Cell 2: selective import.
cat > "$TMPDIR/selective.ae" <<'EOF'
import std.os (system)

main() {
    rc = system("echo hi")
}
EOF
out2="$(AETHER_HOME="$ROOT" "$AETHERC" --emit=ast "$TMPDIR/selective.ae" 2>/dev/null)"
rc2=$?
[ "$rc2" -eq 0 ] && { echo "  [PASS] 2a exit 0"; pass=$((pass+1)); } || { echo "  [FAIL] 2a exit nonzero"; fail=$((fail+1)); }
assert_contains "2b selected list emitted" '"selected":["system"]' "$out2"
assert_contains "2c bare call canonicalises to os_system" '"callee":"os_system"' "$out2"

# Cell 3: bare extern.
cat > "$TMPDIR/bare_extern.ae" <<'EOF'
extern syscall(n: int) -> int

main() {
    rc = syscall(0)
}
EOF
out3="$(AETHER_HOME="$ROOT" "$AETHERC" --emit=ast "$TMPDIR/bare_extern.ae" 2>/dev/null)"
rc3=$?
[ "$rc3" -eq 0 ] && { echo "  [PASS] 3a exit 0"; pass=$((pass+1)); } || { echo "  [FAIL] 3a exit nonzero"; fail=$((fail+1)); }
assert_contains "3b extern_function emitted" '"kind":"extern_function"' "$out3"
assert_contains "3c extern name is syscall" '"name":"syscall"' "$out3"
assert_not_contains "3d non-variadic extern has no variadic flag" '"variadic":true' "$out3"

# Cell 4: variadic extern.
cat > "$TMPDIR/variadic_extern.ae" <<'EOF'
extern dprintf(fd: int, fmt: string, ...) -> int

main() {
    dprintf(1, "hi\n")
}
EOF
out4="$(AETHER_HOME="$ROOT" "$AETHERC" --emit=ast "$TMPDIR/variadic_extern.ae" 2>/dev/null)"
rc4=$?
[ "$rc4" -eq 0 ] && { echo "  [PASS] 4a exit 0"; pass=$((pass+1)); } || { echo "  [FAIL] 4a exit nonzero"; fail=$((fail+1)); }
assert_contains "4b variadic flag present" '"variadic":true' "$out4"

# Cell 5: glob import.
cat > "$TMPDIR/glob_import.ae" <<'EOF'
import std.os (*)

main() {
    rc = system("echo hi")
}
EOF
out5="$(AETHER_HOME="$ROOT" "$AETHERC" --emit=ast "$TMPDIR/glob_import.ae" 2>/dev/null)"
rc5=$?
[ "$rc5" -eq 0 ] && { echo "  [PASS] 5a exit 0"; pass=$((pass+1)); } || { echo "  [FAIL] 5a exit nonzero"; fail=$((fail+1)); }
assert_contains "5b glob flag present" '"glob":true' "$out5"

# Cell 6: per-node file + line populated. Use the qualified probe.
assert_contains "6a file field present" '"file":"' "$out1"
assert_contains "6b line field present" '"line":' "$out1"

# Cell 7: parse failure → non-zero exit.
cat > "$TMPDIR/syntax_err.ae" <<'EOF'
this is not valid aether code === ===
EOF
out7="$(AETHER_HOME="$ROOT" "$AETHERC" --emit=ast "$TMPDIR/syntax_err.ae" 2>/dev/null)"
rc7=$?
if [ "$rc7" -ne 0 ]; then
    echo "  [PASS] 7 parse failure → non-zero exit ($rc7)"
    pass=$((pass + 1))
else
    echo "  [FAIL] 7 parse failure but exit was 0"
    fail=$((fail + 1))
fi

# Cell 8: typecheck failure → non-zero exit.
cat > "$TMPDIR/typecheck_err.ae" <<'EOF'
main() {
    rc = nonexistent_function(0)
}
EOF
out8="$(AETHER_HOME="$ROOT" "$AETHERC" --emit=ast "$TMPDIR/typecheck_err.ae" 2>/dev/null)"
rc8=$?
if [ "$rc8" -ne 0 ]; then
    echo "  [PASS] 8 typecheck failure → non-zero exit ($rc8)"
    pass=$((pass + 1))
else
    echo "  [FAIL] 8 typecheck failure but exit was 0"
    fail=$((fail + 1))
fi

# Cell 9: empty (function-only) file → exit 0, nodes array contains only
# the synthetic main bookkeeping (no imports, no externs, no user calls).
cat > "$TMPDIR/empty.ae" <<'EOF'
main() {
}
EOF
out9="$(AETHER_HOME="$ROOT" "$AETHERC" --emit=ast "$TMPDIR/empty.ae" 2>/dev/null)"
rc9=$?
if [ "$rc9" -eq 0 ]; then
    echo "  [PASS] 9a exit 0 on empty file"
    pass=$((pass + 1))
else
    echo "  [FAIL] 9a exit was non-zero on empty file"
    fail=$((fail + 1))
fi
assert_contains "9b output is a JSON object with nodes key" '"nodes":' "$out9"

# --------------------------------------------------------------
# Cell 10-15: function_call args[] — literal vs computed.
# Per veto-emit-ast-args.md (aeb-side ask): each function_call
# node carries args[], one entry per positional/named arg in
# source order. Literal args (string/int/float/bool) emit
# {"literal": "<value>"}; anything else emits
# {"computed": true}. Trailing closures / DSL bodies are NOT
# args and are filtered out.
# --------------------------------------------------------------

# Cell 10: single string literal arg.
cat > "$TMPDIR/arg_string_literal.ae" <<'EOF'
extern do_stuff(coord: string) -> int
main() {
    rc = do_stuff("org.foo:bar:1.2.3")
}
EOF
out10="$(AETHER_HOME="$ROOT" "$AETHERC" --emit=ast "$TMPDIR/arg_string_literal.ae" 2>/dev/null)"
assert_contains "10 string literal arg" '"args":[{"literal":"org.foo:bar:1.2.3"}]' "$out10"

# Cell 11: int literal arg.
cat > "$TMPDIR/arg_int_literal.ae" <<'EOF'
extern with_port(p: int) -> int
main() {
    rc = with_port(8080)
}
EOF
out11="$(AETHER_HOME="$ROOT" "$AETHERC" --emit=ast "$TMPDIR/arg_int_literal.ae" 2>/dev/null)"
assert_contains "11 int literal arg" '"args":[{"literal":"8080"}]' "$out11"

# Cell 12: multiple positional literals.
cat > "$TMPDIR/arg_multi_literal.ae" <<'EOF'
extern multi(a: string, b: int, c: string) -> int
main() {
    rc = multi("first", 42, "third")
}
EOF
out12="$(AETHER_HOME="$ROOT" "$AETHERC" --emit=ast "$TMPDIR/arg_multi_literal.ae" 2>/dev/null)"
assert_contains "12 multi literal args" '"args":[{"literal":"first"},{"literal":"42"},{"literal":"third"}]' "$out12"

# Cell 13: identifier reference is computed, not literal —
# even when the identifier itself binds a literal value,
# aeb's "never allow on computed arg" rule requires the
# distinction. The variable ref is not a literal expression.
cat > "$TMPDIR/arg_identifier.ae" <<'EOF'
extern do_stuff(s: string) -> int
main() {
    user_input = "static-text"
    rc = do_stuff(user_input)
}
EOF
out13="$(AETHER_HOME="$ROOT" "$AETHERC" --emit=ast "$TMPDIR/arg_identifier.ae" 2>/dev/null)"
assert_contains "13 identifier ref is computed" '"args":[{"computed":true}]' "$out13"

# Cell 14: mixed literal + computed args, in source order.
cat > "$TMPDIR/arg_mixed.ae" <<'EOF'
extern do_stuff(coord: string, key: string, count: int) -> int
main() {
    dyn = "from-env"
    rc = do_stuff("static-coord", dyn, 5)
}
EOF
out14="$(AETHER_HOME="$ROOT" "$AETHERC" --emit=ast "$TMPDIR/arg_mixed.ae" 2>/dev/null)"
assert_contains "14 mixed literal+computed in order" \
    '"args":[{"literal":"static-coord"},{"computed":true},{"literal":"5"}]' "$out14"

# Cell 15: const-folded numeric arg arrives as a literal —
# `do_stuff(1 + 2)` folds to `do_stuff(3)` before the emit
# walk runs (post-optimize_ast). Per veto-emit-ast-args.md
# sub-ask 0.
cat > "$TMPDIR/arg_fold.ae" <<'EOF'
extern do_stuff(n: int) -> int
main() {
    rc = do_stuff(1 + 2)
}
EOF
out15="$(AETHER_HOME="$ROOT" "$AETHERC" --emit=ast "$TMPDIR/arg_fold.ae" 2>/dev/null)"
assert_contains "15 const-folded arg arrives as literal" '"args":[{"literal":"3"}]' "$out15"

# Cell 16: zero-arg call still emits an empty args array.
cat > "$TMPDIR/arg_empty.ae" <<'EOF'
extern do_stuff() -> int
main() {
    rc = do_stuff()
}
EOF
out16="$(AETHER_HOME="$ROOT" "$AETHERC" --emit=ast "$TMPDIR/arg_empty.ae" 2>/dev/null)"
assert_contains "16 zero-arg call has empty args array" '"args":[]' "$out16"

echo ""
if [ "$fail" -eq 0 ]; then
    echo "  [PASS] aetherc_emit_ast: $pass/$pass"
    exit 0
else
    echo "  [FAIL] aetherc_emit_ast: $pass passed, $fail failed"
    exit 1
fi
