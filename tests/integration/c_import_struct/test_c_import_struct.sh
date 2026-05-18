#!/bin/sh
# Integration test for "P0: Header-Defined C Struct Interop"
# (docs/redis-porting-language-gaps.md).
#
# Asserts that `extern struct Name @c_import { ... }`:
#   1. builds and runs — fields read/write correctly through a raw
#      `ptr` cast to `*client`;
#   2. emits NO `typedef struct client` of its own (the C header
#      client.h owns it; a competing typedef would be a C error);
#   3. survives an Aether module import (`cdefs` declares it, `probe`
#      imports it).
#
# client.h is force-included into the generated C via the
# `cflags = "-include client.h"` in aether.toml. If Aether emitted
# its own `typedef struct client`, the build would fail with a
# duplicate-typedef error.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

build_log="$tmpdir/build.log"
if ! "$AE" build probe.ae -o "$tmpdir/probe" >"$build_log" 2>&1; then
    echo "  [FAIL] c_import_struct: ae build failed"
    sed 's/^/    /' "$build_log" | head -25
    exit 1
fi

# The generated C must NOT contain a typedef for `client` — the
# header owns it. Locate the .gen.c the build produced.
genc="$(find "$tmpdir" -name '*.gen.c' 2>/dev/null | head -1)"
if [ -z "$genc" ]; then
    # Some build layouts keep the .gen.c next to the source.
    genc="$(find "$SCRIPT_DIR" -name 'probe*.c' 2>/dev/null | head -1)"
fi
if [ -n "$genc" ] && grep -Eq 'typedef struct client' "$genc"; then
    echo "  [FAIL] c_import_struct: generated C emitted a typedef for 'client'"
    echo "         (the C header owns the type; Aether must suppress it)"
    exit 1
fi

if ! "$tmpdir/probe" > "$tmpdir/run.out" 2>&1; then
    echo "  [FAIL] c_import_struct: binary failed to run"
    sed 's/^/    /' "$tmpdir/run.out" | head -10
    exit 1
fi

if ! grep -q '^argc=3 fd=17$' "$tmpdir/run.out" || \
   ! grep -q '^OK$' "$tmpdir/run.out"; then
    echo "  [FAIL] c_import_struct: unexpected output"
    sed 's/^/    /' "$tmpdir/run.out" | head -10
    exit 1
fi

echo "  [PASS] c_import_struct"
exit 0
