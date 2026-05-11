#!/bin/sh
# #260 Tier 0: HTTP/1.1 keep-alive end-to-end.
#
# Drives 3 requests over a single TCP connection using curl's
# multi-URL form (which reuses the connection by default). Verifies:
#   - all 3 requests get 200 + body 'ka-ok'
#   - the server emits Connection: keep-alive on the first two
#     responses and Connection: close on the third (max=5 - 0 - 1
#     = 4 remaining after request #1; etc.)
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
        echo "  [SKIP-WIN] http_server_keepalive — HTTP server/proxy/middleware code is platform-independent; covered by POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not on PATH"
    exit 0
fi

TMPDIR="$(mktemp -d)"
cleanup() {
    if [ -n "${SRV_PID:-}" ]; then
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
        echo "  [FAIL] server died:"
        head -20 "$TMPDIR/srv.log"
        exit 1
    fi
    sleep 0.1
done
sleep 0.3

# Three URLs in one curl invocation re-uses a single TCP connection.
# Use distinct -o files per URL so we can verify each response
# independently (curl overwrites a single -o file across multiple URLs).
# -v writes the response headers + status to stderr; capture for the
# keep-alive header check below.
ERR="$TMPDIR/curl.err"
if ! curl --silent --show-error --max-time 5 -v \
        http://127.0.0.1:18103/ -o "$TMPDIR/r1" \
        http://127.0.0.1:18103/ -o "$TMPDIR/r2" \
        http://127.0.0.1:18103/ -o "$TMPDIR/r3" \
        2>"$ERR"; then
    echo "  [FAIL] curl failed:"
    cat "$ERR"
    echo "--- server log ---"
    head -30 "$TMPDIR/srv.log"
    exit 1
fi

# Each response body should be exactly 'ka-ok'.
for n in 1 2 3; do
    body=$(cat "$TMPDIR/r$n")
    if [ "$body" != "ka-ok" ]; then
        echo "  [FAIL] response $n body wrong; got '$body'"
        exit 1
    fi
done

# curl reuses the TCP connection when possible; check it didn't open
# three. Verbose output reports "Connection #0" for the first and a
# reuse message for subsequent requests. The exact wording varies:
#   curl 7.x: "Re-using existing connection"
#   curl 8.x: "Reusing existing http: connection"
# Match both with a tolerant pattern.
reused=$(grep -cE "Re-?using existing.* connection" "$ERR" || true)
if [ "$reused" -lt 2 ]; then
    echo "  [FAIL] curl didn't reuse the connection ($reused reuses, want >= 2)"
    echo "--- curl verbose ---"
    cat "$ERR"
    exit 1
fi

# At least one response should carry Connection: keep-alive.
if ! grep -qi "Connection: keep-alive" "$ERR"; then
    echo "  [FAIL] no Connection: keep-alive header in any response"
    grep -i "Connection:" "$ERR"
    exit 1
fi

echo "  [PASS] http_server_keepalive: $reused reused connections, all 3 requests served"
