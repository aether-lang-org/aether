#!/bin/sh
# Regression for issue #752: heap-string fields of a returned struct
# must survive the (Struct, err) tuple-return boundary. See probe.ae.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

# Build from the test dir so `import lib.rec` resolves against
# lib/rec.ae sitting next to probe.ae.
cd "$SCRIPT_DIR"
if ! "$ROOT/build/ae" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] issue_752_struct_string_tuple: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -15
    exit 1
fi

if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] issue_752_struct_string_tuple: probe exited non-zero"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

if grep -q "^OK issue_752_struct_string_tuple" "$TMPDIR/run.log"; then
    echo "  [PASS] issue_752_struct_string_tuple: 4 cases"
else
    echo "  [FAIL] issue_752_struct_string_tuple: didn't reach OK line"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi
