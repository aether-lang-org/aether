#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="/tmp/ae_528_newline_bracket.ae"
OUT="/tmp/ae_528_newline_bracket.c"

cat > "$SRC" <<'AEOF'
main() {
    data = [10, 20, 30]
    [1 + 1, 99]
    println("OK")
}
AEOF

"$ROOT/build/aetherc" "$SRC" "$OUT"

if grep -Fq 'data[1 + 1' "$OUT"; then
    echo "FAIL: newline-led bracket folded into previous data[...] expression"
    exit 1
fi

if ! grep -Fq '{2, 99};' "$OUT"; then
    echo "FAIL: newline-led array literal statement was not emitted separately"
    exit 1
fi

echo "PASS parser newline bracket"
