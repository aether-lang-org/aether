#!/bin/sh
# std.http.middleware.use_real_ip — leftmost X-Forwarded-For
# extraction.
#
# Verifies:
#   1. A request with `X-Forwarded-For: 1.2.3.4, 10.0.0.1` causes
#      the handler to see X-Real-IP=1.2.3.4 (leftmost = original
#      client per RFC 7239 / X-Forwarded-For convention).
#   2. A request without X-Forwarded-For doesn't get an X-Real-IP
#      header (handler sees ip=).
#   3. Whitespace around the leftmost token is stripped.
#   4. Re-running the middleware against an already-tagged request
#      is idempotent (no duplicate X-Real-IP appended).
#
# Skips cleanly when curl is missing.

# Skip on Windows — the HTTP server / proxy / middleware code path
# under test is platform-independent userland C; the Linux and
# macOS CI matrix entries already exercise every behaviour this
# test asserts. Each curl invocation under MSYS2 bash pays Cygwin
# fork-emulation + Defender + slower-than-POSIX localhost overhead
# (~10-100x POSIX's per-spawn cost), so running these on Windows
# adds minutes to the pipeline without adding coverage. Windows-
# specific runtime concerns (file I/O, process spawning, path
# handling) are covered by fs_*, std_ipc_*, run_lib_path, and
# ae_help tests respectively.
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_real_ip — HTTP server/proxy/middleware code is platform-independent; covered by POSIX matrix"
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

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
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

URL="http://127.0.0.1:18272/whoami"

# Test 1 — multi-hop X-Forwarded-For. Leftmost wins.
RESP1=$(curl --silent --show-error --max-time 5 \
              -H 'X-Forwarded-For: 1.2.3.4, 10.0.0.1, 192.168.1.1' \
              "$URL" 2>"$TMPDIR/c1.err") || {
    echo "  [FAIL] curl 1 failed:"; cat "$TMPDIR/c1.err"; exit 1
}
[ "$RESP1" = "ip=1.2.3.4" ] || {
    echo "  [FAIL] expected ip=1.2.3.4, got '$RESP1'"; exit 1
}

# Test 2 — no X-Forwarded-For. Middleware leaves req unchanged;
# handler sees no X-Real-IP, echoes empty.
RESP2=$(curl --silent --show-error --max-time 5 \
              "$URL" 2>"$TMPDIR/c2.err") || {
    echo "  [FAIL] curl 2 failed:"; cat "$TMPDIR/c2.err"; exit 1
}
[ "$RESP2" = "ip=" ] || {
    echo "  [FAIL] expected ip= (empty), got '$RESP2'"; exit 1
}

# Test 3 — whitespace around the leftmost token is stripped.
RESP3=$(curl --silent --show-error --max-time 5 \
              -H 'X-Forwarded-For:   192.0.2.1   , 10.0.0.5' \
              "$URL" 2>"$TMPDIR/c3.err") || {
    echo "  [FAIL] curl 3 failed:"; cat "$TMPDIR/c3.err"; exit 1
}
[ "$RESP3" = "ip=192.0.2.1" ] || {
    echo "  [FAIL] expected ip=192.0.2.1 (whitespace stripped), got '$RESP3'"; exit 1
}

# Test 4 — single-IP X-Forwarded-For (most common direct-LB case).
RESP4=$(curl --silent --show-error --max-time 5 \
              -H 'X-Forwarded-For: 203.0.113.42' \
              "$URL" 2>"$TMPDIR/c4.err") || {
    echo "  [FAIL] curl 4 failed:"; cat "$TMPDIR/c4.err"; exit 1
}
[ "$RESP4" = "ip=203.0.113.42" ] || {
    echo "  [FAIL] expected ip=203.0.113.42, got '$RESP4'"; exit 1
}

# Test 5 — Idempotency: client already supplies X-Real-IP. Our
# middleware must not append a second one (would mask the first).
# We send X-Real-IP=already-set ALONGSIDE X-Forwarded-For=1.2.3.4;
# the handler should see the original X-Real-IP=already-set, NOT
# the extracted-from-XFF value. (This protects against "client
# spoofs X-Real-IP" only when the trusted edge already strips it;
# the doc warns about this trust model.)
RESP5=$(curl --silent --show-error --max-time 5 \
              -H 'X-Real-IP: already-set' \
              -H 'X-Forwarded-For: 1.2.3.4' \
              "$URL" 2>"$TMPDIR/c5.err") || {
    echo "  [FAIL] curl 5 failed:"; cat "$TMPDIR/c5.err"; exit 1
}
[ "$RESP5" = "ip=already-set" ] || {
    echo "  [FAIL] expected ip=already-set (idempotent), got '$RESP5'"; exit 1
}

echo "  [PASS] real_ip middleware (X-Forwarded-For extraction)"
