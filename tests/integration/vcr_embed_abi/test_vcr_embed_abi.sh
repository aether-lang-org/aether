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
# If python3 is available, also serve the tape over HTTP so the driver
# can exercise aether_vcr_embed_start_playback_url (tape fetched over the
# network, not from disk). Skipped gracefully where python3 is absent.
TAPE_URL=""
PY_PID=""
if command -v python3 >/dev/null 2>&1; then
    ( cd "$SCRIPT_DIR" && exec python3 -m http.server 18137 --bind 127.0.0.1 ) \
        >/dev/null 2>&1 &
    PY_PID=$!
    trap 'rm -rf "$TMPDIR"; [ -n "$PY_PID" ] && kill "$PY_PID" 2>/dev/null' EXIT
    # Wait for the static server to come up.
    i=0
    while [ $i -lt 30 ]; do
        if curl -fsS "http://127.0.0.1:18137/smoke.tape" >/dev/null 2>&1; then break; fi
        i=$((i + 1)); sleep 0.1
    done
    TAPE_URL="http://127.0.0.1:18137/smoke.tape"
fi

if ! OUT="$("$TMPDIR/consume" "$TMPDIR/libvcr$LIB_EXT" "$SCRIPT_DIR/smoke.tape" $TAPE_URL 2>&1)"; then
    echo "$OUT"; fail "VCR embed C driver reported an error"
fi
echo "$OUT" | grep -q "^OK:" || { echo "$OUT"; fail "unexpected driver output"; }

if [ -n "$TAPE_URL" ]; then
    echo "$OUT" | grep -q "start_playback_url round-trip" \
        || { echo "$OUT"; fail "start_playback_url round-trip did not run"; }
    echo "  [PASS] vcr_embed_abi: --emit=lib playback round-trips via dlopen (disk + HTTP tape)"
else
    echo "  [PASS] vcr_embed_abi: --emit=lib playback round-trips via dlopen (disk tape; python3 absent, URL mode skipped)"
fi
