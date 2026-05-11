#!/bin/sh
# #260 Tier 0 / Phase C3: parallel keep-alive sessions through the
# drain helper. Drives 4 parallel curl clients, each issuing 25
# requests over its own TCP connection, and verifies all 100
# responses arrive cleanly with no cross-session pollution.

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
        echo "  [SKIP-WIN] http_server_actor_dispatch — HTTP server/proxy/middleware code is platform-independent; covered by POSIX matrix"
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

PORT=18104
# Wait for the server to ACTUALLY accept connections, not just print
# READY. The server prints READY at line 50 of server.ae but doesn't
# call http_server_start_raw until the SrvActor receives StartSrv —
# which on slower runners (Windows GHA) can be ~hundreds of ms after
# READY hits the log. Probe the port directly until curl succeeds.
#
# 30s window: 5s→15s was Paul's Windows-runner bump in 12c0db4.
# 15s started failing again on Windows GHA — bind logs "Server
# running at..." but accept() takes longer than expected for the
# first connection. Bumped to 30s to match http_server_h2_tls and
# kept the diagnostic on failure so the underlying Windows
# accept-loop slowness can be investigated when it surfaces.
deadline=$(($(date +%s) + 30))
last_curl_err="$TMPDIR/curl_probe.err"
while [ "$(date +%s)" -lt "$deadline" ]; do
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        echo "  [FAIL] server died:"
        head -20 "$TMPDIR/srv.log"
        exit 1
    fi
    if curl -s -o /dev/null --max-time 1 \
            "http://127.0.0.1:$PORT/echo" 2>"$last_curl_err"; then
        break
    fi
    sleep 0.1
done
if [ "$(date +%s)" -ge "$deadline" ]; then
    echo "  [FAIL] server never accepted connections within 30s"
    echo "--- last curl error ---"
    cat "$last_curl_err" 2>/dev/null
    echo "--- server log ---"
    head -30 "$TMPDIR/srv.log"
    exit 1
fi

# Drive 4 parallel sessions, each with 25 requests over one TCP
# connection. Each session uses a unique X-Session-Id; the server
# echoes it back. The runner verifies every response carries the
# right session ID with no cross-pollution.
REQS_PER_SESSION=25
SESSIONS="alpha beta gamma delta"

run_session() {
    sid="$1"
    out="$TMPDIR/out-$sid"
    : > "$out"
    # Multi-URL curl re-uses one TCP connection by default.
    # Distinct -o files per URL so each response stays separate.
    args=""
    n=0
    while [ "$n" -lt "$REQS_PER_SESSION" ]; do
        args="$args http://127.0.0.1:18104/echo -o $out.$n"
        n=$((n + 1))
    done
    # shellcheck disable=SC2086
    curl --silent --show-error --max-time 10 \
        -H "X-Session-Id: $sid" \
        $args 2>"$TMPDIR/curl-$sid.err"
}

# Start all 4 in parallel.
for sid in $SESSIONS; do
    run_session "$sid" &
done
wait

# Verify every response from every session contains the right
# echoed session ID and nothing else.
fail=0
for sid in $SESSIONS; do
    n=0
    while [ "$n" -lt "$REQS_PER_SESSION" ]; do
        f="$TMPDIR/out-$sid.$n"
        if [ ! -s "$f" ]; then
            echo "  [FAIL] session $sid request $n: empty/missing response"
            fail=1
            break
        fi
        body=$(cat "$f")
        if [ "$body" != "echo:$sid" ]; then
            echo "  [FAIL] session $sid request $n: expected 'echo:$sid', got '$body'"
            fail=1
            break
        fi
        n=$((n + 1))
    done
done

if [ "$fail" -ne 0 ]; then
    echo "--- server log ---"
    head -30 "$TMPDIR/srv.log"
    exit 1
fi

total=$((4 * REQS_PER_SESSION))
echo "  [PASS] http_server_actor_dispatch: $total parallel keep-alive responses, no cross-session pollution"
