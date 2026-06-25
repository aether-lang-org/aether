#!/bin/sh
# Regression: issue #870 — a SELECTIVE `import std.string (...)` in the entry
# file must not suppress the qualified `string.X` call surface for code merged
# in from an imported module that itself bare-imports std.string.
#
# Before the fix, the entry's selective import marked the `string` namespace
# selective for the whole merged unit, so a qualified `string.concat(...)` in
# the imported module (and `string.from_int(...)` in the entry) was rejected
# with E0301 "Undefined function 'string.concat'". The merge now injects a
# synthetic bare import for each module's bare imports, re-opening the
# qualified surface for the merged unit.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] issue870: $AE not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
cd "$tmpdir" || exit 1

mkdir -p lib/aeo_compose
cat > lib/aeo_compose/module.ae <<'EOF'
exports (orchestration)
import std.string
orchestration() -> string { return string.concat("x", "y") }   // qualified call
EOF

cat > run.ae <<'EOF'
import aeo_compose (orchestration)
import std.string (string_length)                              // SELECTIVE
main() { println(string.from_int(string_length(orchestration()))) }
EOF

if ! "$AE" build run.ae -o out --lib lib >"$tmpdir/build.log" 2>&1; then
    echo "  [FAIL] issue870: build failed (qualified string.X suppressed by selective import)"
    cat "$tmpdir/build.log"
    exit 1
fi

if [ -x ./out ]; then
    out="$(./out 2>&1)"
elif [ -x ./out.exe ]; then
    out="$(./out.exe 2>&1)"
else
    echo "  [FAIL] issue870: built binary not found"
    ls -la
    exit 1
fi

# orchestration() = "xy" (len 2) -> from_int -> "2"
if [ "$out" != "2" ]; then
    echo "  [FAIL] issue870: expected '2', got '$out'"
    exit 1
fi

echo "  [PASS] issue870: selective import keeps qualified string.X from merged modules"
exit 0
