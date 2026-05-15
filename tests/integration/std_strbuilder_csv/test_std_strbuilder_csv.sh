#!/bin/sh
# Integration test: std.strbuilder v2 realistic combo. A CSV emitter
# that uses append / append_byte / append_format / truncate (row
# cancellation) / clear (buffer reuse) and the binary-safe
# finish_with_length handoff together. Verifies byte-exact output and
# bounds RSS across 2000 build/finish_with_length/free cycles.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] std_strbuilder_csv: ae not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

if ! "$AE" build probe.ae -o "$tmpdir/probe" >"$tmpdir/build.log" 2>&1; then
    echo "  [FAIL] std_strbuilder_csv: build failed"
    sed 's/^/    /' "$tmpdir/build.log" | head -30
    exit 1
fi

if ! "$tmpdir/probe" >"$tmpdir/run.log" 2>&1; then
    echo "  [FAIL] std_strbuilder_csv: probe exited non-zero"
    sed 's/^/    /' "$tmpdir/run.log" | head -20
    exit 1
fi

if ! grep -q "all std_strbuilder_csv cases passed" "$tmpdir/run.log"; then
    echo "  [FAIL] std_strbuilder_csv: probe output missing success marker"
    sed 's/^/    /' "$tmpdir/run.log" | head -20
    exit 1
fi

echo "  [PASS] std_strbuilder_csv: byte-exact CSV + 2000-cycle RSS bound"
exit 0
