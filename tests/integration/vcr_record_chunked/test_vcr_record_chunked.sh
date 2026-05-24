#!/bin/sh
# Integration test: VCR record mode against a Transfer-Encoding: chunked
# upstream (vcr_record_chunked_dechunk_wish.md). A raw-socket C upstream
# replies chunked (the Aether server can't — it always sets
# Content-Length); the Aether driver asserts the client de-chunks and
# that record→replay round-trips the decoded payload (not chunk framing).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] vcr_record_chunked on Windows (raw POSIX sockets)"
        exit 0
        ;;
esac

TMPDIR="$(mktemp -d)"
UP_PID=""
cleanup() { [ -n "$UP_PID" ] && kill "$UP_PID" 2>/dev/null; rm -rf "$TMPDIR"; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

# Build + start the chunked upstream; it prints its OS-assigned port.
cc "$SCRIPT_DIR/chunked_upstream.c" -o "$TMPDIR/upstream" 2>"$TMPDIR/cc.log" \
    || { cat "$TMPDIR/cc.log"; fail "could not compile chunked_upstream.c"; }
"$TMPDIR/upstream" > "$TMPDIR/port.txt" 2>/dev/null &
UP_PID=$!

PORT=""
i=0
while [ "$i" -lt 100 ]; do
    PORT="$(head -n1 "$TMPDIR/port.txt" 2>/dev/null)"
    [ -n "$PORT" ] && break
    sleep 0.05
    i=$((i + 1))
done
[ -n "$PORT" ] || fail "chunked upstream did not report a port"

OUT="$(UPSTREAM_PORT="$PORT" AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/vcr_chunked.ae" 2>&1)"
if ! echo "$OUT" | grep -q "^PASS"; then
    echo "$OUT"
    fail "VCR chunked record/replay assertions did not pass"
fi

echo "  [PASS] vcr_record_chunked: client de-chunks + record/replay store decoded payload"
