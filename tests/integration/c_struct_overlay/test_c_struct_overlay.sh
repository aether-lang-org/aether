#!/bin/sh
# #891 @c_struct typed overlay: read/write C structs by field name against
# explicit offsets, with the COMPILER choosing the accessor width (killing
# the get_long-on-uint32 footgun). Verifies:
#   - width-derived accessors in the generated C (uint32 field -> _uint32);
#   - cumulative offsets for nested overlays;
#   - runtime round-trip of scalar + nested + the s->length-- shape;
#   - no `import std.mem` required.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) echo "  [SKIP] c_struct_overlay on Windows"; exit 0 ;;
esac
[ -x "$AE" ] || { echo "  [SKIP] ae not built"; exit 0; }
TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
fail=0

# 1. Generated C uses the width-derived accessor for the uint32 field.
AETHER_HOME="$ROOT" "$AETHERC" "$SCRIPT_DIR/probe.ae" "$TMPDIR/probe.c" >/dev/null 2>&1 || {
    echo "  [FAIL] aetherc failed on probe.ae"; exit 1; }
if grep -q 'aether_mem_set_uint32((void\*)(s), 16' "$TMPDIR/probe.c"; then
    echo "  [PASS] uint32 field -> width-correct aether_mem_set_uint32 at offset 16"
else
    echo "  [FAIL] expected aether_mem_set_uint32 at offset 16 (the width-footgun fix)"; fail=1
fi
# nested cumulative offset: last_id.ms is at 24, last_id.seq at 32
if grep -q 'aether_mem_set_long((void\*)(s), 32' "$TMPDIR/probe.c"; then
    echo "  [PASS] nested overlay cumulative offset (last_id.seq @ 24+8=32)"
else
    echo "  [FAIL] expected nested offset 32 for last_id.seq"; fail=1
fi

# 2. Runtime round-trip.
ACTUAL=$(AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/probe.ae" 2>&1)
EXPECT="length=999999
slen=42
last=111/222
maxdel_ms=111"
if [ "$ACTUAL" = "$EXPECT" ]; then
    echo "  [PASS] runtime round-trip (scalar + nested + s->length--)"
else
    echo "  [FAIL] runtime mismatch:"; echo "$ACTUAL" | sed 's/^/      /'; fail=1
fi

[ "$fail" -eq 0 ] && echo "  c_struct_overlay: OK"
[ "$fail" -eq 0 ]
