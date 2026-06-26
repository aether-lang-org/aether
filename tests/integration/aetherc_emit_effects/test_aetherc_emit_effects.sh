#!/bin/sh
# Verifies `aetherc --emit=effects` emits the DERIVED per-function
# effect/purity analysis as JSON for external auditors (aeb's
# supply-chain veto). See issue #889.
#
# Contract under test:
#   - derived, NOT author-tag-asserted (an attacker omits the tag);
#   - whole-program transitive (through helpers AND imported modules);
#   - per-function;
#   - fail-closed on a raw `extern` (treat as reaches-everything).
#
# Cells:
#   1. A pure arithmetic fn → pure:true, reaches:[].
#   2. A fn calling os.system directly → reaches:["os"], no tag needed.
#   3. A fn reaching os TRANSITIVELY via a helper → reaches:["os"].
#   4. A fn reaching a raw extern → extern:true, reaches all caps, pure:false.
#   5. A fn reaching os THROUGH AN IMPORTED MODULE → reaches:["os"].

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] aetherc_emit_effects on Windows"; exit 0 ;;
esac
[ -x "$AETHERC" ] || { echo "  [SKIP] aetherc not built"; exit 0; }

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
pass=0; fail=0

assert_contains() {
    label="$1"; needle="$2"; haystack="$3"
    if echo "$haystack" | grep -qF "$needle"; then
        echo "  [PASS] $label"; pass=$((pass + 1))
    else
        echo "  [FAIL] $label — '$needle' not in:"; echo "$haystack" | sed 's/^/      /'
        fail=$((fail + 1))
    fi
}

# Cells 1-4: single file.
cat > "$TMPDIR/evil.ae" <<'EOF'
import std.os

add(a: int, b: int) -> int { return a + b }
helper() -> int { return os.system("ls") }
sneaky() -> int { return helper() }
extern raw_thing(x: int) -> int
viaextern() -> int { return raw_thing(5) }
main() { println("${add(1, 2)}") }
EOF

OUT=$(AETHER_HOME="$ROOT" "$AETHERC" --emit=effects "$TMPDIR/evil.ae" "$TMPDIR/evil.c" 2>&1)
assert_contains "pure arithmetic fn"        '"add": { "pure": true, "extern": false, "reaches": [] }' "$OUT"
assert_contains "direct os.system reaches os" '"helper": { "pure": false, "extern": false, "reaches": ["os"] }' "$OUT"
assert_contains "transitive os via helper (no tag)" '"sneaky": { "pure": false, "extern": false, "reaches": ["os"] }' "$OUT"
assert_contains "extern fail-closed reaches all + impure" '"viaextern": { "pure": false, "extern": true, "reaches": ["fs", "net", "os"] }' "$OUT"

# Cell 5: transitive through an IMPORTED module.
mkdir -p "$TMPDIR/osmod"
cat > "$TMPDIR/osmod/module.ae" <<'EOF'
import std.os
exports (do_exec)
do_exec(c: string) -> int { return os.system(c) }
EOF
cat > "$TMPDIR/cross.ae" <<'EOF'
import osmod
calls_imported() -> int { return osmod.do_exec("ls") }
main() { println("${calls_imported()}") }
EOF
OUT2=$(cd "$TMPDIR" && AETHER_HOME="$ROOT" "$AETHERC" --emit=effects cross.ae cross.c 2>&1)
assert_contains "transitive os through imported module" '"calls_imported": { "pure": false, "extern": false, "reaches": ["os"] }' "$OUT2"

echo "  --- $pass passed, $fail failed ---"
[ "$fail" -eq 0 ]
