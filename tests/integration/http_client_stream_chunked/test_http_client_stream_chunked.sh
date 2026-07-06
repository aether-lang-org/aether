#!/bin/sh
# Issue #1004: streaming a Transfer-Encoding: chunked response body. main.ae
# stands up a hand-written chunked HTTP/1.1 server over raw TCP and streams its
# body with the client, asserting the chunk decoder reassembles "hello world"
# (never exposing chunk framing). Acceptance: prints "PASS:" and exits 0.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] http_client_stream_chunked: program exited $rc"
    echo "$out" | head -30 | sed 's/^/          /'
    exit 1
fi
if printf '%s\n' "$out" | grep -q 'FAIL'; then
    echo "  [FAIL] http_client_stream_chunked:"
    printf '%s\n' "$out" | grep 'FAIL' | sed 's/^/          /'
    exit 1
fi
if ! printf '%s\n' "$out" | grep -q 'PASS:'; then
    echo "  [FAIL] http_client_stream_chunked: no PASS line"
    echo "$out" | head -30 | sed 's/^/          /'
    exit 1
fi
echo "  [PASS] http_client_stream_chunked: chunked response body streams + decodes"
exit 0
