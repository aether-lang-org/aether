#!/bin/sh
# Issue #1004: streaming HTTP response bodies. The client can pull a large
# download body window-by-window (peak memory = one window) instead of
# buffering the whole thing. main.ae is a self-contained differential test: it
# serves a 128 KiB body from an in-process server and fetches it both buffered
# (send_request + response_body) and streamed (send_stream + response_read
# loop), asserting the streamed reassembly is byte-identical to the buffered
# body across many read windows.
#
# Acceptance: the program prints a single "PASS: streamed ... byte-identical to
# buffered" line and exits 0. Any mismatch (length, bytes, stream flag, or a
# mid-stream error) fails with a FAIL line and a non-zero exit.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?

if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] http_client_stream: program exited $rc"
    echo "$out" | head -30 | sed 's/^/          /'
    exit 1
fi
if printf '%s\n' "$out" | grep -q 'FAIL'; then
    echo "  [FAIL] http_client_stream:"
    printf '%s\n' "$out" | grep 'FAIL' | sed 's/^/          /'
    exit 1
fi
if ! printf '%s\n' "$out" | grep -q 'PASS: streamed'; then
    echo "  [FAIL] http_client_stream: no PASS line"
    echo "$out" | head -30 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] http_client_stream: response body streams byte-identical to the buffered fetch"
exit 0
