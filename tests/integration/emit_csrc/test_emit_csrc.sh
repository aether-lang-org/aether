#!/bin/sh
# Regression: aether#996 — `ae build --emit=csrc` emits portable C source + a
# catalog header, and NOT a native lib.
#
# Asserts:
#   - `ae build --emit=csrc -o out` produces out.c and out.h (no gcc, no .so)
#   - out.h declares the aether_<name> catalog prototypes
#   - out.c compiles+links against `ae cflags` into a working .so that EXPORTS
#     the catalog symbols (proving the emitted source is genuinely usable)

# Skip on Windows — compiles the emitted .c with the POSIX toolchain here; the
# emit itself is platform-independent.
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] test_emit_csrc on Windows (compiles emitted .c via POSIX toolchain)"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v gcc >/dev/null 2>&1 && ! command -v cc >/dev/null 2>&1; then
    echo "  [SKIP] no C compiler on PATH"
    exit 0
fi
CC="$(command -v gcc || command -v cc)"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

OUT="$TMPDIR/out"
if ! "$AE" build --emit=csrc "$SCRIPT_DIR/mylib.ae" -o "$OUT" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] ae build --emit=csrc failed:"; cat "$TMPDIR/build.log" | head -10; exit 1
fi

# .c and .h must exist; no .so.
[ -f "$OUT.c" ] || { echo "  [FAIL] no $OUT.c emitted"; exit 1; }
[ -f "$OUT.h" ] || { echo "  [FAIL] no $OUT.h emitted"; exit 1; }
# NB: written as `if` not `[ ... ] && { ... }` — under `set -e` a false `[ ]`
# test at statement level aborts the script (with no output). The `.so`-absent
# case is the PASSING one, so it must not look like a failed command.
if [ -f "$OUT.so" ]; then
    echo "  [FAIL] --emit=csrc unexpectedly produced a .so"; exit 1
fi

# Header declares the catalog prototypes.
grep -q 'aether_add' "$OUT.h" || { echo "  [FAIL] out.h missing aether_add prototype"; cat "$OUT.h"; exit 1; }
grep -q 'aether_greet' "$OUT.h" || { echo "  [FAIL] out.h missing aether_greet prototype"; exit 1; }

# The emitted C compiles + links against the runtime into a .so that exports
# the catalog symbols.
CFLAGS="$("$AE" cflags 2>/dev/null)"
if ! $CC -fPIC -shared "$OUT.c" $CFLAGS -o "$TMPDIR/lib.so" >"$TMPDIR/cc.log" 2>&1; then
    echo "  [FAIL] emitted .c did not compile against ae cflags:"; cat "$TMPDIR/cc.log" | head -15; exit 1
fi
if command -v nm >/dev/null 2>&1; then
    # Portable symbol dump. `nm -D` is GNU-only — macOS/BSD nm REJECTS it, and a
    # failing command substitution under `set -e` aborts the whole script (with
    # no output), so each `$(...)` must be `|| true`-guarded or it never reaches
    # the fallback. macOS also lists dynamic symbols with plain nm and mangles C
    # names with a leading underscore, so match `aether_add` with an optional one.
    syms="$(nm -D "$TMPDIR/lib.so" 2>/dev/null || true)"
    [ -n "$syms" ] || syms="$(nm -gU "$TMPDIR/lib.so" 2>/dev/null || true)"
    [ -n "$syms" ] || syms="$(nm "$TMPDIR/lib.so" 2>/dev/null || true)"
    if ! printf '%s\n' "$syms" | grep -Eq '(^|[^A-Za-z0-9_])_?aether_add([^A-Za-z0-9_]|$)'; then
        echo "  [FAIL] built .so does not export aether_add"
        printf '%s\n' "$syms" | grep -i aether_ | head
        exit 1
    fi
fi

echo "  [PASS] emit_csrc: .c + catalog .h emitted, compiles+links, exports resolve"
exit 0
