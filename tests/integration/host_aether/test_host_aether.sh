#!/bin/sh
# contrib/host/aether end-to-end smoke test.
#
# The Aether-hosts-Aether bridge compiles a child .ae to a native binary
# (shelling out to the ae/aetherc driver via AETHER_AE_PATH) and runs it
# as a subprocess. This test exercises the simplest, portable rung:
# compile + run a trivial child, no sandbox, check exit code 0.
#
# Skips cleanly unless the toolchain is present:
#   - build/ae must be built
#   - AETHER_AE_PATH must point at the ae driver (the bridge's compile
#     step reads it; we default it to build/ae here)
# CI machines that haven't built ae yet no-op rather than fail.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] host_aether: $AE not built"
    exit 0
fi

# Build the aether-host bridge .a into the dir ae build's host-bridge
# scanner reads. The bridge needs only libc + the in-tree sandbox runtime,
# so this always compiles where ae itself builds.
OUT="$ROOT/build/contrib"
mkdir -p "$OUT"
if ! cc -c -O2 -DAETHER_HAS_AETHER_HOST -DAETHER_HAS_SANDBOX -I"$ROOT" \
        "$ROOT/contrib/host/aether/aether_host_aether.c" \
        -o "$OUT/host_aether.o" 2>/tmp/host_aether_cc.err; then
    echo "  [FAIL] host_aether: bridge compile failed"
    head -10 /tmp/host_aether_cc.err
    exit 1
fi
ar rcs "$OUT/libaether_host_aether.a" "$OUT/host_aether.o"

# The bridge's aether_host_compile shells out to the ae driver via
# AETHER_AE_PATH; point it at the freshly built ae.
export AETHER_AE_PATH="$AE"
export AETHER_HOST_CHILD="$SCRIPT_DIR/child.ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/uses_aether.ae" \
        >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] host_aether: ae run exited non-zero"
    head -20 "$TMPDIR/err.log"
    head -10 "$ACTUAL"
    exit 1
fi

if grep -Fxq "PASS: aether host compile + run round-trip" "$ACTUAL"; then
    echo "  [PASS] contrib.host.aether: compile + run round-trip"
else
    echo "  [FAIL] host_aether: driver did not reach PASS"
    echo "--- actual ---"; cat "$ACTUAL"
    echo "--- stderr ---"; cat "$TMPDIR/err.log"
    exit 1
fi
