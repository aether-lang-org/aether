#!/bin/sh
# #260 Tier 3 (Phase F1 + F2): access logger + Prometheus metrics
# end-to-end. Drives a few requests, then verifies:
#   - the JSON access log captured one line per request with the
#     right status / path / duration fields
#   - /metrics returns Prometheus text format with the expected
#     counters (requests_total, errors_total) and histogram buckets
#
# Skips when curl is missing.

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
        echo "  [SKIP-WIN] http_server_observability — HTTP server/proxy/middleware code is platform-independent; covered by POSIX matrix"
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
LOG="$TMPDIR/access.log"
cleanup() {
    if [ -n "${SRV_PID:-}" ]; then
        kill "$SRV_PID" 2>/dev/null || true
        wait "$SRV_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

AETHER_HOME="$ROOT" ACCESS_LOG_PATH="$LOG" \
    "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
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

URL="http://127.0.0.1:18108"

# Drive a few requests across both routes.
curl -s -o /dev/null --max-time 5 "$URL/api/data"  >/dev/null
curl -s -o /dev/null --max-time 5 "$URL/api/data"  >/dev/null
curl -s -o /dev/null --max-time 5 "$URL/api/data"  >/dev/null
curl -s -o /dev/null --max-time 5 "$URL/api/error" >/dev/null

# Give the access log file a moment to flush.
sleep 0.2

# --- Access log: at least 4 JSON lines, each with method/path/status ---
if [ ! -f "$LOG" ]; then
    echo "  [FAIL] access log file not created at $LOG"
    exit 1
fi
lines=$(wc -l < "$LOG" | tr -d ' ')
if [ "$lines" -lt 4 ]; then
    echo "  [FAIL] expected >= 4 access-log lines; got $lines"
    cat "$LOG"
    exit 1
fi
if ! grep -q '"method":"GET"' "$LOG"; then
    echo "  [FAIL] access log missing method:GET"
    cat "$LOG"
    exit 1
fi
if ! grep -q '"path":"/api/data"' "$LOG"; then
    echo "  [FAIL] access log missing path:/api/data"
    cat "$LOG"
    exit 1
fi
if ! grep -q '"status":200' "$LOG"; then
    echo "  [FAIL] access log missing status:200"
    cat "$LOG"
    exit 1
fi
if ! grep -q '"status":500' "$LOG"; then
    echo "  [FAIL] access log missing status:500"
    cat "$LOG"
    exit 1
fi

# --- Metrics: scrape /metrics and verify counters + histograms ---
M="$TMPDIR/metrics.body"
curl -s -o "$M" --max-time 5 "$URL/metrics" >/dev/null
if ! grep -q '^# TYPE aether_http_requests_total counter' "$M"; then
    echo "  [FAIL] metrics missing TYPE header for requests_total"
    head -20 "$M"
    exit 1
fi
if ! grep -q 'aether_http_requests_total{method="GET",path="/api/data"}' "$M"; then
    echo "  [FAIL] metrics missing requests_total for /api/data"
    head -20 "$M"
    exit 1
fi
if ! grep -q 'aether_http_errors_total{method="GET",path="/api/error"}' "$M"; then
    echo "  [FAIL] metrics missing errors_total for /api/error"
    head -20 "$M"
    exit 1
fi
if ! grep -q 'aether_http_request_duration_seconds_bucket' "$M"; then
    echo "  [FAIL] metrics missing histogram buckets"
    head -20 "$M"
    exit 1
fi

echo "  [PASS] http_server_observability: access log JSON + Prometheus metrics both correct"
