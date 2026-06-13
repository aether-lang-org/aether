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
# Asserts:
#   1. POSITIVE — aetherc's .gen.c for probe.ae compiles with typed.h
#      force-included: no conflicting-declaration error.
#   2. The emitted prototypes use the exact spellings (const void* /
#      const char* / char*), not bare void*.
#   3. NEGATIVE — a drifted signature (drift.ae, one param dropped),
#      compiled with the same header force-included, MUST fail with a
#      conflicting-types error.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ] || [ ! -x "$AE" ]; then
    echo "  [SKIP] c_qualified_ptr: aetherc/ae not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
cd "$SCRIPT_DIR" || exit 1

# Portable include paths for compiling a generated .gen.c, straight from
# the toolchain itself (the dynamic -I walker behind `ae cflags`).
incs="$("$AE" cflags 2>/dev/null | tr ' ' '\n' | grep '^-I' | tr '\n' ' ')"
CC="${CC:-cc}"
warnsoff="-Wno-unused-parameter -Wno-unused-function -Wno-unused-but-set-variable"

# (1)+(2) POSITIVE: emit C, compile with the header force-included.
if ! "$AETHERC" probe.ae "$tmpdir/probe.c" >"$tmpdir/emit.log" 2>&1; then
    echo "  [FAIL] c_qualified_ptr: aetherc failed on probe.ae"
    sed 's/^/    /' "$tmpdir/emit.log" | head -20
    exit 1
fi

# (2) exact spellings must appear in the emitted prototypes.
for needle in 'const void\*' 'const char\*' 'char\*'; do
    if ! grep -Eq "$needle" "$tmpdir/probe.c"; then
        echo "  [FAIL] c_qualified_ptr: emitted C missing spelling: $needle"
        exit 1
    fi
done
# aesort's owned prototype must be the exact qualified form, not bare void*.
if ! grep -Eq 'void aesort\(void\*, size_t, size_t, const void\*, size_t, size_t\)' "$tmpdir/probe.c"; then
    echo "  [FAIL] c_qualified_ptr: aesort prototype not emitted in exact qualified form"
    grep -n 'aesort' "$tmpdir/probe.c" | head -3 | sed 's/^/    /'
    exit 1
fi

# (1) compile-only against the force-included header: must be clean.
if ! "$CC" -std=c11 $incs -I"$SCRIPT_DIR" $warnsoff \
        -include typed.h -c "$tmpdir/probe.c" -o "$tmpdir/probe.o" \
        >"$tmpdir/cc.log" 2>&1; then
    echo "  [FAIL] c_qualified_ptr: .gen.c did NOT compile against its header (conflicting prototype?)"
    sed 's/^/    /' "$tmpdir/cc.log" | head -20
    exit 1
fi

# (3) NEGATIVE: a drifted signature must be REJECTED by the C compiler.
if ! "$AETHERC" drift.ae "$tmpdir/drift.c" >"$tmpdir/emit2.log" 2>&1; then
    echo "  [FAIL] c_qualified_ptr: aetherc failed on drift.ae"
    sed 's/^/    /' "$tmpdir/emit2.log" | head -20
    exit 1
fi
if "$CC" -std=c11 $incs -I"$SCRIPT_DIR" $warnsoff \
        -include typed.h -c "$tmpdir/drift.c" -o "$tmpdir/drift.o" \
        >"$tmpdir/cc2.log" 2>&1; then
    echo "  [FAIL] c_qualified_ptr: a drifted signature compiled clean — the"
    echo "         header cross-check is not catching prototype drift"
    exit 1
fi

echo "  [PASS] c_qualified_ptr"
exit 0
