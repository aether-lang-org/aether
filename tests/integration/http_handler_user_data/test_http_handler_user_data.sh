#!/bin/sh
# #633 regression: a non-zero `user_data` ptr passed to
# http_server_get reaches the handler. The C plumbing always worked
# end-to-end; the downstream fbs-core port concluded the slot was
# unusable from Aether because no example showed the
# struct-ptr → user_data pattern (the examples all thread `0`). This
# test pins the working pattern and serves as the worked example
# for the docs reference (docs/http-handler-context.md).

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_handler_user_data — HTTP server is platform-independent; covered by POSIX matrix"
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

BASE="http://127.0.0.1:18299"

# Test 1 — route /a sees its own ctx (label=alpha db_id=11).
RESP=$(curl --silent --show-error --max-time 5 "$BASE/a") || {
    echo "  [FAIL] curl /a failed"; exit 1
}
EXPECT="a: label=alpha db_id=11"
[ "$RESP" = "$EXPECT" ] || {
    echo "  [FAIL] /a: expected '$EXPECT', got '$RESP'"; exit 1
}

# Test 2 — route /b sees ITS own ctx (label=beta db_id=22). Critically
# DIFFERENT from /a; proves per-route user_data isolation.
RESP=$(curl --silent --show-error --max-time 5 "$BASE/b") || {
    echo "  [FAIL] curl /b failed"; exit 1
}
EXPECT="b: label=beta db_id=22"
[ "$RESP" = "$EXPECT" ] || {
    echo "  [FAIL] /b: expected '$EXPECT', got '$RESP'"; exit 1
}

# Test 3 — route /n sees ud == null (the existing `0` pattern keeps
# working).
RESP=$(curl --silent --show-error --max-time 5 "$BASE/n") || {
    echo "  [FAIL] curl /n failed"; exit 1
}
[ "$RESP" = "ud=null" ] || {
    echo "  [FAIL] /n: expected 'ud=null', got '$RESP'"; exit 1
}

# Test 4 — re-call /a after /b to prove the slots stay bound to
# their original handler. Was nervous about handler-pointer aliasing
# or last-write-wins semantics; this pins per-route ownership.
RESP=$(curl --silent --show-error --max-time 5 "$BASE/a") || {
    echo "  [FAIL] curl /a (re) failed"; exit 1
}
[ "$RESP" = "a: label=alpha db_id=11" ] || {
    echo "  [FAIL] /a (re): expected stable 'a: label=alpha db_id=11', got '$RESP'"
    exit 1
}

echo "  [PASS] http_handler_user_data: per-route user_data reaches handler (#633)"
