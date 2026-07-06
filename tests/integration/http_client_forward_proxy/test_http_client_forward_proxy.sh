#!/bin/sh
# Regression: aether#1012 part 2 — std.http.client forward-proxy control.
#
# Starts an Aether origin (:18120) + a python forward proxy (:18121), then runs
# an Aether client (client.ae) that asserts:
#   - use_http_proxy routes via the proxy
#   - ignore_http_proxy goes direct
#   - use_env_proxy refuses a loopback-IP $HTTP_PROXY (SSRF guard)
#
# Skips cleanly without python3.

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_client_forward_proxy — proxy/socket code is platform-independent; covered by POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v python3 >/dev/null 2>&1; then
    echo "  [SKIP] python3 not on PATH"
    exit 0
fi

TMPDIR="$(mktemp -d)"
cleanup() {
    [ -n "${ORIGIN_PID:-}" ] && kill "$ORIGIN_PID" 2>/dev/null || true
    [ -n "${PROXY_PID:-}" ]  && kill "$PROXY_PID"  2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# Start the forward proxy.
python3 "$SCRIPT_DIR/proxy.py" 18121 >"$TMPDIR/proxy.log" 2>&1 &
PROXY_PID=$!

# Start the Aether origin, wait for READY.
AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/origin.ae" >"$TMPDIR/origin.log" 2>&1 &
ORIGIN_PID=$!
deadline=$(($(date +%s) + 30))
while [ "$(date +%s)" -lt "$deadline" ]; do
    grep -q READY "$TMPDIR/origin.log" 2>/dev/null && break
    if ! kill -0 "$ORIGIN_PID" 2>/dev/null; then
        echo "  [FAIL] origin died before READY:"; head -20 "$TMPDIR/origin.log"; exit 1
    fi
    sleep 0.1
done
grep -q READY "$TMPDIR/origin.log" 2>/dev/null || { echo "  [FAIL] origin never READY"; head -20 "$TMPDIR/origin.log"; exit 1; }
sleep 0.3

# Run the client with a loopback-IP HTTP_PROXY so case 3 can exercise the SSRF
# guard. Cases 1 and 2 pin/ignore the proxy explicitly and don't consult env.
OUT="$TMPDIR/client.out"
if ! AETHER_HOME="$ROOT" HTTP_PROXY="http://127.0.0.1:18121" \
        "$AE" run "$SCRIPT_DIR/client.ae" >"$OUT" 2>&1; then
    echo "  [FAIL] client exited non-zero:"; cat "$OUT" | head -10; exit 1
fi

if grep -q '^PASS ' "$OUT"; then
    echo "  [PASS] http_client_forward_proxy: explicit-via-proxy, ignore-direct, env-SSRF-blocked"
    exit 0
fi

echo "  [FAIL] client did not report PASS:"; cat "$OUT" | head -10
exit 1
