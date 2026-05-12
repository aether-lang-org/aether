#!/bin/sh
# Regression: codegen must emit `(void)` rather than `()` for zero-arg
# functions so that `-Wstrict-prototypes` (a default warning on modern
# gcc/clang) doesn't fire. Without the fix, every Aether no-arg function
# emits an unprototyped declaration — invisible under a permissive
# warning set, immediately fatal under -Werror=strict-prototypes.
#
# Three call-site shapes are exercised because the codegen change
# touched three places: forward declarations, extern declarations, and
# function definitions.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] strict_prototypes: aetherc not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cat > "$tmpdir/src.ae" <<'AE'
extern external_noarg() -> int

defined_noarg() -> int {
    return 42
}

main() {
    x = defined_noarg()
    println(x)
}
AE

if ! "$AETHERC" "$tmpdir/src.ae" "$tmpdir/src.c" >"$tmpdir/aetherc.log" 2>&1; then
    echo "  [FAIL] strict_prototypes: aetherc rejected the source"
    cat "$tmpdir/aetherc.log"
    exit 1
fi

# Any `<ident>()` form in a declaration or definition would be an
# unprototyped function. The fix replaces these with `<ident>(void)`.
# Grep for the forbidden shape, allowing `int main()` to remain (it
# isn't legally affected by -Wstrict-prototypes the same way and we
# don't touch the main-emission path).
bad="$(grep -nE '^[a-zA-Z_][a-zA-Z0-9_* ]*[a-zA-Z_0-9]+\(\)[ ;{]' "$tmpdir/src.c" \
        | grep -v 'int main(' || true)"
if [ -n "$bad" ]; then
    echo "  [FAIL] strict_prototypes: emitted C has unprototyped zero-arg function(s):"
    echo "$bad" | sed 's/^/    /'
    exit 1
fi

# Positive assertion: each of the three sites we touched must appear
# with `(void)` — forward decl, extern decl, and definition.
for needle in \
        'int defined_noarg(void);' \
        'int external_noarg(void);' \
        'int defined_noarg(void) {'
do
    if ! grep -qF "$needle" "$tmpdir/src.c"; then
        echo "  [FAIL] strict_prototypes: expected '$needle' in emitted C"
        echo "    --- emitted C (head): ---"
        head -160 "$tmpdir/src.c" | sed 's/^/    /'
        exit 1
    fi
done

# Belt-and-braces: compile the emitted C with -Werror=strict-prototypes
# to prove the warning category actually stays silent. Skip if no cc.
if command -v cc >/dev/null 2>&1; then
    if ! cc -c -Werror=strict-prototypes -I"$ROOT" -I"$ROOT/runtime" \
            -I"$ROOT/runtime/actors" -I"$ROOT/runtime/scheduler" \
            -I"$ROOT/runtime/utils" -I"$ROOT/runtime/memory" \
            -I"$ROOT/runtime/config" -I"$ROOT/std" -I"$ROOT/std/string" \
            -I"$ROOT/std/io" -I"$ROOT/std/math" \
            "$tmpdir/src.c" -o "$tmpdir/src.o" >"$tmpdir/cc.log" 2>&1; then
        # Only count strict-prototypes hits as failure — other errors
        # (missing headers under this isolated compile) are noise.
        if grep -q 'strict-prototypes' "$tmpdir/cc.log"; then
            echo "  [FAIL] strict_prototypes: cc -Werror=strict-prototypes flagged emitted C"
            grep 'strict-prototypes' "$tmpdir/cc.log" | sed 's/^/    /'
            exit 1
        fi
    fi
fi

echo "  [PASS] strict_prototypes: zero-arg functions emit (void) at all three sites"
exit 0
