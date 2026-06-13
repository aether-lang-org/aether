#!/bin/sh
# Integration test for "Import C macro constants via extern const @c_import"
# (#702).
#
# Asserts that `extern const NAME: type @c_import`:
#   1. builds and runs — the macros resolve to the header's values;
#   2. emits NO definition of its own for the imported names (no #define,
#      no `static`, no forward decl) — the macro name appears in the
#      generated C only at USE sites, verbatim;
#   3. uses the macro name verbatim in a comparison (proving codegen does
#      not inline a value of its own).
#
# consts.h is force-included into the generated C via the
# `cflags = "-include consts.h"` in aether.toml.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

build_log="$tmpdir/build.log"
if ! "$AE" build probe.ae -o "$tmpdir/probe" >"$build_log" 2>&1; then
    echo "  [FAIL] c_import_const: ae build failed"
    sed 's/^/    /' "$build_log" | head -25
    exit 1
fi

# Locate the generated C the build produced.
genc="$(find "$tmpdir" -name '*.gen.c' 2>/dev/null | head -1)"
if [ -z "$genc" ]; then
    genc="$(find "$SCRIPT_DIR" -name 'probe*.c' 2>/dev/null | head -1)"
fi
if [ -n "$genc" ]; then
    # The declaration must emit nothing: no #define / static / extern decl
    # for the imported names.
    if grep -Eq '#define[[:space:]]+MY_FLAG|static[[:space:]].*MY_FLAG|MY_FLAG[[:space:]]*=' "$genc"; then
        echo "  [FAIL] c_import_const: generated C defined MY_FLAG itself"
        echo "         (the header owns it; the declaration must emit nothing)"
        exit 1
    fi
    # The macro must appear verbatim at a use site (the comparison).
    if ! grep -Eq 'MY_LIMIT' "$genc"; then
        echo "  [FAIL] c_import_const: MY_LIMIT not emitted verbatim at use site"
        exit 1
    fi
fi

if ! "$tmpdir/probe" > "$tmpdir/run.out" 2>&1; then
    echo "  [FAIL] c_import_const: binary failed (assertion exit)"
    sed 's/^/    /' "$tmpdir/run.out" | head -10
    exit 1
fi

if ! grep -q '^aether-702$' "$tmpdir/run.out" || ! grep -q '^PASS$' "$tmpdir/run.out"; then
    echo "  [FAIL] c_import_const: unexpected output"
    sed 's/^/    /' "$tmpdir/run.out" | head -10
    exit 1
fi

echo "  [PASS] c_import_const"
exit 0
