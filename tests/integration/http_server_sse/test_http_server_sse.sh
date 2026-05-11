#!/bin/sh
# #260 Tier 2 / Phase E3: Server-Sent Events end-to-end.
#
# Connects to /events with curl -N (disable buffering) and verifies:
#   - Content-Type: text/event-stream header
#   - 3 events received with the expected event names + data
#   - third event carries id: evt-3
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
        echo "  [SKIP-WIN] http_server_sse — HTTP server/proxy/middleware code is platform-independent; covered by POSIX matrix"
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

PORT=18109

# Wait for the server to actually accept connections, not just for
# READY to land in the log. server.ae prints READY before sending
# the StartSrv message that opens the listening socket — on slower
# runners (Linux GHA, Windows GHA) the listener can be hundreds of
# ms behind READY, and a curl issued against the port too early
# returns no response at all (no Content-Type, no body — exactly
# the failure shape this guard is here to prevent). Probe the port
# directly until a HEAD succeeds; bail if the server died before
# binding.
deadline=$(($(date +%s) + 15))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        echo "  [FAIL] server died:"
        head -20 "$TMPDIR/srv.log"
        exit 1
    fi
    if curl -s -o /dev/null --max-time 1 \
            "http://127.0.0.1:$PORT/events" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

OUT="$TMPDIR/sse.out"
HEADERS="$TMPDIR/sse.h"
# -N: no buffering. Server closes after 3 events; max-time bounds
# the wait if it didn't.
curl -s -N -D "$HEADERS" -o "$OUT" --max-time 5 \
    "http://127.0.0.1:$PORT/events" || true

if ! grep -qi "Content-Type: text/event-stream" "$HEADERS"; then
    echo "  [FAIL] missing Content-Type: text/event-stream"
    cat "$HEADERS"
    exit 1
fi

# Expect three events, each with event:tick and matching data.
for n in 1 2 3; do
    if ! grep -q "^event: tick" "$OUT"; then
        echo "  [FAIL] missing 'event: tick' lines"
        cat "$OUT"
        exit 1
    fi
    if ! grep -q "^data: $n$" "$OUT"; then
        echo "  [FAIL] missing 'data: $n'"
        cat "$OUT"
        exit 1
    fi
done

# The third event should also carry id: evt-3.
if ! grep -q "^id: evt-3$" "$OUT"; then
    echo "  [FAIL] missing 'id: evt-3' on the third event"
    cat "$OUT"
    exit 1
fi

echo "  [PASS] http_server_sse: 3 events received with correct framing + id"
