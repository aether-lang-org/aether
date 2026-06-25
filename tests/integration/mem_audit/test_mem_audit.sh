#!/bin/sh
# Regression: issue #868 — `aetherc --audit-mem` lists every raw std.mem
# offset access with the byte width its accessor name implies, so a port
# author can check each read/write width against the C field's actual type.
# The footgun: reading a 4-byte field with get_long (8 bytes) silently pulls
# in adjacent bytes (the issue's 94 TB-alloc crash). The audit surfaces the
# call sites; the width-exact accessors already exist for the fix.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] mem_audit: $AETHERC not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
cd "$tmpdir" || exit 1

# listpackEntry shape: { uchar* sval; uint32 slen@8; long long lval@16 }.
# slen is a 4-byte field; reading it with get_long (8 bytes) is the bug.
cat > probe.ae <<'EOF'
import std.mem
SVAL() -> int { return 0 }
SLEN() -> int { return 8 }
read_wrong(p: ptr) -> long { return mem.get_long(p, SLEN()) }   // 8-byte read of 4-byte field
read_right(p: ptr) -> long { return mem.get_u32_le(p, SLEN()) } // correct
read_ptr(p: ptr) -> ptr   { return mem.get_ptr(p, SVAL()) }
main() { println("ok") }
EOF

out="$("$AETHERC" --audit-mem probe.ae probe.c 2>&1)"

fail=0
echo "$out" | grep -q "mem.get_long reads 8 bytes"   || { echo "  missing: get_long 8-byte report"; fail=1; }
echo "$out" | grep -q "mem.get_u32_le reads 4 bytes" || { echo "  missing: get_u32_le 4-byte report"; fail=1; }
echo "$out" | grep -q "mem.get_ptr reads 8 bytes"    || { echo "  missing: get_ptr report"; fail=1; }
echo "$out" | grep -q "audit-mem: 3 raw std.mem offset accesses found" || { echo "  missing: summary line"; fail=1; }

if [ "$fail" -ne 0 ]; then
    echo "  [FAIL] mem_audit: --audit-mem output incomplete"
    echo "$out"
    exit 1
fi

echo "  [PASS] mem_audit: --audit-mem reports every raw offset access + width"
exit 0
