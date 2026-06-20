#!/bin/sh
# Integration test for `@c_import` against a C header that defines
# its struct WITHOUT a convenience typedef.  This is the shape of
# POSIX's `struct tm` (<time.h>), `struct stat` (<sys/stat.h>),
# `struct sockaddr` (<sys/socket.h>), and many other system structs:
# the layout is named via `struct tag`, no `typedef struct tag tag;`
# is supplied.
#
# Aedis surfaced this porting localtime.c: aetherc was emitting bare
# `tm *` everywhere it rendered a pointer to a `@c_import struct tm`,
# but `<time.h>` doesn't ship a `typedef struct tm tm;` so the
# generated C failed with `unknown type name 'tm'`.  Aetherc now
# emits `struct Name *` for any `@c_import` struct, which always
# works whether the header carries the typedef or not.
#
# The test header (widget.h) deliberately omits the typedef.  The
# probe builds and runs only when aetherc emits `struct widget *`
# wherever it renders a pointer to widget.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

build_log="$tmpdir/build.log"
if ! "$AE" build probe.ae -o "$tmpdir/probe" >"$build_log" 2>&1; then
    echo "  [FAIL] c_import_struct_no_typedef: ae build failed"
    sed 's/^/    /' "$build_log" | head -25
    exit 1
fi

# Sanity: the generated C should reference `struct widget` (the
# typedef-less form), not bare `widget`.
genc="$(find "$tmpdir" -name '*.gen.c' 2>/dev/null | head -1)"
if [ -z "$genc" ]; then
    genc="$(find "$SCRIPT_DIR" -name 'probe*.c' 2>/dev/null | head -1)"
fi
if [ -n "$genc" ]; then
    if grep -Eq 'typedef struct widget' "$genc"; then
        echo "  [FAIL] c_import_struct_no_typedef: emitted a typedef for 'widget'"
        echo "         (the C header owns the type — Aether must suppress)"
        exit 1
    fi
    if ! grep -q 'struct widget' "$genc"; then
        echo "  [FAIL] c_import_struct_no_typedef: no 'struct widget' in genc"
        echo "         (aetherc must emit the struct-prefixed pointer form)"
        exit 1
    fi
fi

if ! "$tmpdir/probe" > "$tmpdir/run.out" 2>&1; then
    echo "  [FAIL] c_import_struct_no_typedef: binary failed to run"
    sed 's/^/    /' "$tmpdir/run.out" | head -10
    exit 1
fi

if ! grep -q '^serial=42 weight=107$' "$tmpdir/run.out" || \
   ! grep -q '^OK$' "$tmpdir/run.out"; then
    echo "  [FAIL] c_import_struct_no_typedef: unexpected output"
    sed 's/^/    /' "$tmpdir/run.out" | head -10
    exit 1
fi

echo "  [PASS] c_import_struct_no_typedef"
exit 0
