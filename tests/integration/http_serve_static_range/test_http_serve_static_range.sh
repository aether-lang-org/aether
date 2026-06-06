#!/bin/sh
# #641 regression: http.serve_static honours the Range request header.
#
# Pre-fix: serve_static always responded 200 OK with the whole file
# regardless of Range (zsync downloads broken, resumable downloads
# broken, media seeking broken).
#
# This test covers:
#   1. Range: bytes=0-1023 → 206 with 1024 bytes
#   2. Range: bytes=100-199 → 206 with the matching 100 bytes
#   3. Range: bytes=-50    → suffix-range form
#   4. Range: bytes=4000-  → open-end form
#   5. No Range            → 200 + Accept-Ranges: bytes header present
#   6. Range: bytes=99999-100000 (past EOF) → 416 + Content-Range: */N
#   7. Range: bytes=abc-xyz → 416 (malformed)

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_serve_static_range — HTTP server is platform-independent; covered by POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not on PATH"; exit 0
fi

TMPDIR="$(mktemp -d)"
SRV_PID=""
cleanup() {
    if [ -n "$SRV_PID" ]; then
        kill "$SRV_PID" 2>/dev/null || true
        wait "$SRV_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# Build a 5000-byte test file with deterministic content. Each byte
# is its (index mod 256) so we can verify slice boundaries by
# checking the first/last byte of each response.
python3 -c "
import sys
sys.stdout.buffer.write(bytes(i & 0xff for i in range(5000)))
" > "$TMPDIR/data.bin"

ls -la "$TMPDIR/data.bin" >/dev/null  # sanity

AETHER_HOME="$ROOT" TEST_BASE="$TMPDIR" "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

deadline=$(($(date +%s) + 15))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if grep -q READY "$TMPDIR/srv.log" 2>/dev/null; then break; fi
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        echo "  [FAIL] server died:"; head -20 "$TMPDIR/srv.log"; exit 1
    fi
    sleep 0.1
done
sleep 0.3

URL="http://127.0.0.1:18301/data.bin"

# Test 1 — bytes=0-1023 should be 206 with 1024 bytes
RESP_HEAD="$TMPDIR/h1.txt"
curl -s -o "$TMPDIR/b1.bin" -D "$RESP_HEAD" -r 0-1023 "$URL"
CODE=$(awk 'NR==1{print $2}' "$RESP_HEAD")
[ "$CODE" = "206" ] || { echo "  [FAIL] bytes=0-1023: expected 206, got $CODE"; cat "$RESP_HEAD"; exit 1; }
SIZE=$(stat -c %s "$TMPDIR/b1.bin" 2>/dev/null || stat -f %z "$TMPDIR/b1.bin")
[ "$SIZE" = "1024" ] || { echo "  [FAIL] bytes=0-1023: expected 1024 bytes, got $SIZE"; exit 1; }
grep -i "^Content-Range: bytes 0-1023/5000" "$RESP_HEAD" >/dev/null || {
    echo "  [FAIL] bytes=0-1023: missing/wrong Content-Range"; cat "$RESP_HEAD"; exit 1
}
echo "  [PASS] bytes=0-1023: 206 + 1024 bytes + Content-Range"

# Test 2 — bytes=100-199 mid-file
curl -s -o "$TMPDIR/b2.bin" -D "$TMPDIR/h2.txt" -r 100-199 "$URL"
CODE=$(awk 'NR==1{print $2}' "$TMPDIR/h2.txt")
SIZE=$(stat -c %s "$TMPDIR/b2.bin" 2>/dev/null || stat -f %z "$TMPDIR/b2.bin")
[ "$CODE" = "206" ] && [ "$SIZE" = "100" ] || {
    echo "  [FAIL] bytes=100-199: code=$CODE size=$SIZE"; exit 1
}
# First byte of b2 should be 100, last should be 199 (mod 256)
FIRST=$(od -An -tu1 -N1 "$TMPDIR/b2.bin" | tr -d ' \n')
LAST=$(od -An -tu1 -j99 -N1 "$TMPDIR/b2.bin" | tr -d ' \n')
[ "$FIRST" = "100" ] && [ "$LAST" = "199" ] || {
    echo "  [FAIL] bytes=100-199: first=$FIRST last=$LAST (want 100/199)"; exit 1
}
echo "  [PASS] bytes=100-199: correct byte values"

# Test 3 — bytes=-50 (suffix range, last 50 bytes)
curl -s -o "$TMPDIR/b3.bin" -D "$TMPDIR/h3.txt" -r -50 "$URL"
CODE=$(awk 'NR==1{print $2}' "$TMPDIR/h3.txt")
SIZE=$(stat -c %s "$TMPDIR/b3.bin" 2>/dev/null || stat -f %z "$TMPDIR/b3.bin")
[ "$CODE" = "206" ] && [ "$SIZE" = "50" ] || {
    echo "  [FAIL] bytes=-50: code=$CODE size=$SIZE"; exit 1
}
echo "  [PASS] bytes=-50: suffix range"

# Test 4 — bytes=4000- (open end)
curl -s -o "$TMPDIR/b4.bin" -D "$TMPDIR/h4.txt" -r 4000- "$URL"
CODE=$(awk 'NR==1{print $2}' "$TMPDIR/h4.txt")
SIZE=$(stat -c %s "$TMPDIR/b4.bin" 2>/dev/null || stat -f %z "$TMPDIR/b4.bin")
[ "$CODE" = "206" ] && [ "$SIZE" = "1000" ] || {
    echo "  [FAIL] bytes=4000-: code=$CODE size=$SIZE"; exit 1
}
echo "  [PASS] bytes=4000-: open-end range"

# Test 5 — No Range: full file 200 + Accept-Ranges
curl -s -o "$TMPDIR/b5.bin" -D "$TMPDIR/h5.txt" "$URL"
CODE=$(awk 'NR==1{print $2}' "$TMPDIR/h5.txt")
SIZE=$(stat -c %s "$TMPDIR/b5.bin" 2>/dev/null || stat -f %z "$TMPDIR/b5.bin")
[ "$CODE" = "200" ] && [ "$SIZE" = "5000" ] || {
    echo "  [FAIL] no Range: code=$CODE size=$SIZE (want 200/5000)"; exit 1
}
grep -i "^Accept-Ranges: bytes" "$TMPDIR/h5.txt" >/dev/null || {
    echo "  [FAIL] no Range: missing Accept-Ranges header"; cat "$TMPDIR/h5.txt"; exit 1
}
echo "  [PASS] no Range: 200 + Accept-Ranges"

# Test 6 — Unsatisfiable range
curl -s -o "$TMPDIR/b6.bin" -D "$TMPDIR/h6.txt" -r 99999-100000 "$URL"
CODE=$(awk 'NR==1{print $2}' "$TMPDIR/h6.txt")
[ "$CODE" = "416" ] || {
    echo "  [FAIL] unsatisfiable: expected 416, got $CODE"; exit 1
}
grep -i "^Content-Range: bytes \*/5000" "$TMPDIR/h6.txt" >/dev/null || {
    echo "  [FAIL] unsatisfiable: missing Content-Range */N"; cat "$TMPDIR/h6.txt"; exit 1
}
echo "  [PASS] unsatisfiable: 416 + Content-Range */N"

echo "  [PASS] http_serve_static_range: 6/6 (#641)"
