#!/bin/sh
# Test: --emit=lib --with=net produces a loadable shared object even when
# the program pulls in a TLS-using runtime object (std.http's embedded
# server). Regression guard for the PIC runtime fix
# (vcr_embed_abi_wish.md Part A): before it, the link failed with
# `relocation R_X86_64_TPOFF32 ... recompile with -fPIC`.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] test_emit_lib_net on Windows (uses POSIX dlopen)"
        exit 0
        ;;
    Darwin) LIB_EXT=".dylib" ;;
    *)      LIB_EXT=".so" ;;
esac

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

# Build the std.http-importing module as a shared library.
if ! AETHER_HOME="$ROOT" "$ROOT/build/ae" build --emit=lib --with=net \
        "$SCRIPT_DIR/netmod.ae" -o "$TMPDIR/libnetmod$LIB_EXT" \
        >"$TMPDIR/build.log" 2>&1; then
    echo "--- build log:"; cat "$TMPDIR/build.log"
    fail "ae build --emit=lib --with=net (PIC link regression?)"
fi
[ -f "$TMPDIR/libnetmod$LIB_EXT" ] || fail "no shared object produced"

# Compile the C driver and run it against the .so.
if ! gcc "$SCRIPT_DIR/consume.c" -ldl -o "$TMPDIR/consume" 2>"$TMPDIR/gcc.log"; then
    echo "--- gcc log:"; cat "$TMPDIR/gcc.log"; fail "could not compile consume.c"
fi
if ! OUT="$("$TMPDIR/consume" "$TMPDIR/libnetmod$LIB_EXT" 2>&1)"; then
    echo "$OUT"; fail "C driver could not dlopen/call the .so"
fi
echo "$OUT" | grep -q "^OK:" || { echo "$OUT"; fail "unexpected driver output"; }

echo "  [PASS] emit_lib_net: --emit=lib --with=net links + dlopens a std.http module"
