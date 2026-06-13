#!/bin/sh
# http.request_remote_addr — the trusted TCP peer address (from
# getpeername(2)), exposed to handlers. Distinct from
# X-Forwarded-For; the new accessor must always read the kernel's
# view of the socket, never the client-supplied header.
#
# Verifies:
#   1. Loopback connection → peer=127.0.0.1
#   2. A spoofed X-Forwarded-For does NOT influence the value
#      (the handler echoes both; xff travels through, peer doesn't)
#
# Skips on Windows for the same reason http_real_ip does — the
# server code under test is platform-independent userland C, and
# the curl-over-MSYS2 path is many times slower per spawn.

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_request_remote_addr — covered by POSIX matrix"
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
        echo "  [FAIL] server died:"; head -30 "$TMPDIR/srv.log"; exit 1
    fi
    sleep 0.1
done
sleep 0.3

URL="http://127.0.0.1:18293/whoami"

# Test 1 — plain loopback request. peer should be 127.0.0.1; xff empty.
RESP1=$(curl --silent --show-error --max-time 5 "$URL" 2>"$TMPDIR/c1.err") || {
    echo "  [FAIL] curl 1 failed:"; cat "$TMPDIR/c1.err"; exit 1
}
[ "$RESP1" = "peer=127.0.0.1|xff=" ] || {
    echo "  [FAIL] expected 'peer=127.0.0.1|xff=', got '$RESP1'"; exit 1
}

# Test 2 — spoofed X-Forwarded-For. peer stays 127.0.0.1 (kernel
# truth); xff carries the spoof. This is the load-bearing property
# for in-app allow-lists — a client cannot influence remote_addr.
RESP2=$(curl --silent --show-error --max-time 5 \
              -H 'X-Forwarded-For: 1.2.3.4' \
              "$URL" 2>"$TMPDIR/c2.err") || {
    echo "  [FAIL] curl 2 failed:"; cat "$TMPDIR/c2.err"; exit 1
}
[ "$RESP2" = "peer=127.0.0.1|xff=1.2.3.4" ] || {
    echo "  [FAIL] expected 'peer=127.0.0.1|xff=1.2.3.4', got '$RESP2'"; exit 1
}

echo "  [PASS] http_request_remote_addr (kernel peer, unspoofable by headers)"
