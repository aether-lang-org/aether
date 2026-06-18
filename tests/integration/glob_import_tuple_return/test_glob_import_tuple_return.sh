#!/bin/sh
# Regression (fbs-core ask #1): `import std.fs (*)` must carry the tuple
# return types of the (value, err) wrappers. Before the fix the glob form
# mis-typed them as int and the destructure failed to compile. See
# probe.ae.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! "$ROOT/build/ae" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] glob_import_tuple_return: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -20
    exit 1
fi

if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] glob_import_tuple_return: probe exited non-zero"
    sed 's/^/    /' "$TMPDIR/run.log" | head -20
    exit 1
fi

if grep -q "PASS: glob import tuple-return types correct" "$TMPDIR/run.log"; then
    echo "  [PASS] glob_import_tuple_return: glob carries wrapper tuple types"
else
    echo "  [FAIL] glob_import_tuple_return: didn't reach PASS line"
    sed 's/^/    /' "$TMPDIR/run.log" | head -20
    exit 1
fi
