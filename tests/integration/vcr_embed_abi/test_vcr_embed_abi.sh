#!/bin/sh
# Integration test for the VCR embedding C-ABI (vcr_embed_abi_wish.md
# Part B): build std/http/server/vcr/embed.ae as a shared library
# (--emit=lib --with=fs,net — exercises the PIC runtime from Part A),
# then dlopen it from a C driver and drive a playback round-trip
# (start_playback -> port/base_url/tape_length -> raw-socket GET -> stop).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] vcr_embed_abi on Windows (uses POSIX dlopen + sockets)"
        exit 0
        ;;
    Darwin) LIB_EXT=".dylib" ;;
    *)      LIB_EXT=".so" ;;
esac

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

# Build the embed module as a shared library.
if ! AETHER_HOME="$ROOT" "$AE" build --emit=lib --with=fs,net \
        "$ROOT/std/http/server/vcr/embed.ae" -o "$TMPDIR/libvcr$LIB_EXT" \
        >"$TMPDIR/build.log" 2>&1; then
    echo "--- build log:"; cat "$TMPDIR/build.log"
    fail "ae build --emit=lib --with=fs,net embed.ae"
fi
[ -f "$TMPDIR/libvcr$LIB_EXT" ] || fail "no shared object produced"

# Compile + run the C driver against it.
if ! gcc "$SCRIPT_DIR/consume.c" -ldl -o "$TMPDIR/consume" 2>"$TMPDIR/gcc.log"; then
    echo "--- gcc log:"; cat "$TMPDIR/gcc.log"; fail "could not compile consume.c"
fi
if ! OUT="$("$TMPDIR/consume" "$TMPDIR/libvcr$LIB_EXT" "$SCRIPT_DIR/smoke.tape" 2>&1)"; then
    echo "$OUT"; fail "VCR embed C driver reported an error"
fi
echo "$OUT" | grep -q "^OK:" || { echo "$OUT"; fail "unexpected driver output"; }

echo "  [PASS] vcr_embed_abi: embed.ae --emit=lib playback round-trips via dlopen"
