#!/bin/sh
# Integration test for "P0: Typed And Qualified C Pointers" (#703).
#
# The decisive guarantee #703 asks for: an Aether-owned public symbol,
# and an extern of a const-taking C function, emit prototypes that match
# their C header EXACTLY — so the generated .gen.c compiles with the
# header FORCE-INCLUDED, raising no conflicting-declaration error, which
# restores C's cross-check that the definition still matches every
# caller's prototype. (The existing tests/regression/test_c_typed_pointers.ae
# only proves the spellings parse/typecheck/run; it never compiles a
# prototype against a force-included header. This closes that gap.)
#
# Compilation goes through `ae build` so the .gen.c is built with the
# real toolchain flags (force-include via aether.toml's cflags), rather
# than a hand-rolled cc line that could drift from how Aether actually
# compiles (feature-test macros, std level, include paths).
#
# Asserts:
#   1. POSITIVE — `ae build probe.ae` succeeds with typed.h force-included:
#      no conflicting-declaration error.
#   2. The emitted prototypes use the exact spellings (const void* /
#      const char* / char*) and aesort's exact qualified form.
#   3. NEGATIVE — a drifted signature (drift.ae, one param dropped), built
#      with the same header force-included, MUST fail to compile.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] || [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] c_qualified_ptr: ae/aetherc not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
cd "$SCRIPT_DIR" || exit 1

# (2) Emit C (no header needed) and assert the exact spellings appear.
if ! "$AETHERC" probe.ae "$tmpdir/probe.c" >"$tmpdir/emit.log" 2>&1; then
    echo "  [FAIL] c_qualified_ptr: aetherc failed on probe.ae"
    sed 's/^/    /' "$tmpdir/emit.log" | head -20
    exit 1
fi
for needle in 'const void\*' 'const char\*' 'char\*'; do
    if ! grep -Eq "$needle" "$tmpdir/probe.c"; then
        echo "  [FAIL] c_qualified_ptr: emitted C missing spelling: $needle"
        exit 1
    fi
done
if ! grep -Eq 'void aesort\(void\*, size_t, size_t, const void\*, size_t, size_t\)' "$tmpdir/probe.c"; then
    echo "  [FAIL] c_qualified_ptr: aesort prototype not emitted in exact qualified form"
    grep -n 'aesort' "$tmpdir/probe.c" | head -3 | sed 's/^/    /'
    exit 1
fi

# (1) POSITIVE: build with typed.h force-included (via aether.toml). A
# conflicting prototype would fail this compile.
if ! "$AE" build probe.ae -o "$tmpdir/probe" >"$tmpdir/build.log" 2>&1; then
    echo "  [FAIL] c_qualified_ptr: .gen.c did NOT compile against its header (conflicting prototype?)"
    sed 's/^/    /' "$tmpdir/build.log" | head -25
    exit 1
fi

# (3) NEGATIVE: a drifted signature must be REJECTED by the C compiler.
# Build it in an isolated dir that reuses the same aether.toml + header,
# with drift.ae standing in as `probe.ae` so the [[bin]] entry applies.
negdir="$tmpdir/neg"
mkdir -p "$negdir"
cp drift.ae "$negdir/probe.ae"
cp typed.h "$negdir/typed.h"
cp aether.toml "$negdir/aether.toml"
if ( cd "$negdir" && "$AE" build probe.ae -o "$negdir/probe" ) >"$tmpdir/build2.log" 2>&1; then
    echo "  [FAIL] c_qualified_ptr: a drifted signature compiled clean — the"
    echo "         header cross-check is not catching prototype drift"
    exit 1
fi

echo "  [PASS] c_qualified_ptr"
exit 0
