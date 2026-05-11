#!/bin/sh
# std.http.middleware Bearer + session-cookie auth — end-to-end.
#
# Verifies (Bearer):
#   1. Missing Authorization                  -> 401 + bare Bearer challenge
#   2. Empty token ("Bearer ")                -> 401 + error="invalid_token"
#   3. Wrong token                            -> 401 + error="invalid_token"
#   4. Correct token                          -> 200 "bearer-ok"
#   5. Wrong scheme ("Basic ...")             -> 401 + bare Bearer challenge
#
# Verifies (session-cookie):
#   6. No Cookie header                       -> 302 to /login
#   7. Wrong cookie value                     -> 302 to /login
#   8. Correct cookie value                   -> 200 "session-ok"
#   9. Cookie set among others (RFC 6265 mux) -> 200 "session-ok"
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
        echo "  [SKIP-WIN] http_auth — HTTP server/proxy/middleware code is platform-independent; covered by POSIX matrix"
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

cd "$SCRIPT_DIR"

if ! AETHER_HOME="$ROOT" "$AE" build server.ae -o "$TMPDIR/server" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] http_auth: build failed:"
    head -40 "$TMPDIR/build.log"
    exit 1
fi

# Two strategies, two servers, two pids. Wait until both report READY
# before exercising either; that way port collisions or startup
# failures fail loudly rather than as flaky curl errors.
SRV_BEARER_PID=""
SRV_SESSION_PID=""
cleanup() {
    for pid in "$SRV_BEARER_PID" "$SRV_SESSION_PID" "$SRV_PID"; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

AETHER_HOME="$ROOT" "$TMPDIR/server" bearer  >"$TMPDIR/bearer.log"  2>&1 &
SRV_BEARER_PID=$!
AETHER_HOME="$ROOT" "$TMPDIR/server" session >"$TMPDIR/session.log" 2>&1 &
SRV_SESSION_PID=$!

wait_ready() {
    pid="$1"; log="$2"; tag="$3"
    deadline=$(($(date +%s) + 15))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if grep -q READY "$log" 2>/dev/null; then return 0; fi
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "  [FAIL] $tag server died:"; head -20 "$log"; exit 1
        fi
        sleep 0.1
    done
    echo "  [FAIL] $tag server never READY:"; head -20 "$log"; exit 1
}
wait_ready "$SRV_BEARER_PID"  "$TMPDIR/bearer.log"  bearer
wait_ready "$SRV_SESSION_PID" "$TMPDIR/session.log" session
sleep 0.3

URL_BEARER="http://127.0.0.1:18273/api/whoami"
URL_SESSION="http://127.0.0.1:18274/app/me"

# Helper — fetch HTTP status + WWW-Authenticate header.
status_and_auth() {
    curl --silent --show-error --max-time 5 -o "$TMPDIR/body" \
         -w '%{http_code}\n' -D "$TMPDIR/hdrs" "$@" 2>"$TMPDIR/err" || {
        echo "STATUS_CURL_FAIL"
        return
    }
}

# ---- Bearer ----

# Test 1 — no Authorization header.
RESP=$(status_and_auth "$URL_BEARER")
[ "$RESP" = "401" ] || { echo "  [FAIL] B1 status: expected 401, got $RESP"; exit 1; }
grep -qi 'WWW-Authenticate: Bearer realm="api"' "$TMPDIR/hdrs" || {
    echo "  [FAIL] B1 challenge: missing bare Bearer realm"; cat "$TMPDIR/hdrs"; exit 1;
}
# Bare challenge must NOT carry an `error` param.
if grep -qi 'WWW-Authenticate:.*error=' "$TMPDIR/hdrs"; then
    echo "  [FAIL] B1 challenge: unexpected error param on missing-credential 401"
    exit 1
fi

# Test 2 — empty token after "Bearer ".
RESP=$(status_and_auth -H 'Authorization: Bearer ' "$URL_BEARER")
[ "$RESP" = "401" ] || { echo "  [FAIL] B2 status: expected 401, got $RESP"; exit 1; }
grep -qi 'WWW-Authenticate:.*error="invalid_token"' "$TMPDIR/hdrs" || {
    echo "  [FAIL] B2 challenge: missing error=\"invalid_token\""; cat "$TMPDIR/hdrs"; exit 1;
}

# Test 3 — wrong token.
RESP=$(status_and_auth -H 'Authorization: Bearer wrong-token' "$URL_BEARER")
[ "$RESP" = "401" ] || { echo "  [FAIL] B3 status: expected 401, got $RESP"; exit 1; }
grep -qi 'WWW-Authenticate:.*error="invalid_token"' "$TMPDIR/hdrs" || {
    echo "  [FAIL] B3 challenge: missing error=\"invalid_token\""; exit 1;
}

# Test 4 — correct token.
RESP=$(status_and_auth -H 'Authorization: Bearer good-token' "$URL_BEARER")
[ "$RESP" = "200" ] || { echo "  [FAIL] B4 status: expected 200, got $RESP"; exit 1; }
[ "$(cat "$TMPDIR/body")" = "bearer-ok" ] || {
    echo "  [FAIL] B4 body: expected bearer-ok, got $(cat "$TMPDIR/body")"; exit 1;
}

# Test 5 — wrong scheme. Treat as missing Bearer — bare challenge.
RESP=$(status_and_auth -H 'Authorization: Basic Zm9vOmJhcg==' "$URL_BEARER")
[ "$RESP" = "401" ] || { echo "  [FAIL] B5 status: expected 401, got $RESP"; exit 1; }
if grep -qi 'WWW-Authenticate:.*error=' "$TMPDIR/hdrs"; then
    echo "  [FAIL] B5 challenge: bad scheme should yield bare challenge, not error="
    exit 1
fi

# ---- Session cookie ----

# Test 6 — no Cookie header. Expect 302 -> /login.
RESP=$(status_and_auth "$URL_SESSION")
[ "$RESP" = "302" ] || { echo "  [FAIL] S6 status: expected 302, got $RESP"; exit 1; }
grep -qi 'Location: /login' "$TMPDIR/hdrs" || {
    echo "  [FAIL] S6 redirect: missing Location: /login"; cat "$TMPDIR/hdrs"; exit 1;
}

# Test 7 — wrong cookie. Same redirect.
RESP=$(status_and_auth -H 'Cookie: SESSIONID=bogus' "$URL_SESSION")
[ "$RESP" = "302" ] || { echo "  [FAIL] S7 status: expected 302, got $RESP"; exit 1; }

# Test 8 — correct cookie.
RESP=$(status_and_auth -H 'Cookie: SESSIONID=valid-sid' "$URL_SESSION")
[ "$RESP" = "200" ] || { echo "  [FAIL] S8 status: expected 200, got $RESP"; exit 1; }
[ "$(cat "$TMPDIR/body")" = "session-ok" ] || {
    echo "  [FAIL] S8 body: expected session-ok, got $(cat "$TMPDIR/body")"; exit 1;
}

# Test 9 — RFC 6265 multi-cookie header. SESSIONID is in the middle.
RESP=$(status_and_auth -H 'Cookie: theme=dark; SESSIONID=valid-sid; lang=en' "$URL_SESSION")
[ "$RESP" = "200" ] || { echo "  [FAIL] S9 status: expected 200, got $RESP"; exit 1; }
[ "$(cat "$TMPDIR/body")" = "session-ok" ] || {
    echo "  [FAIL] S9 body: expected session-ok, got $(cat "$TMPDIR/body")"; exit 1;
}

echo "  [PASS] http_auth: Bearer + session-cookie strategies"
