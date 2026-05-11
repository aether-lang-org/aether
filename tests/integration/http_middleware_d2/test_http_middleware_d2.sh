#!/bin/sh
# #260 Tier 1 / Phase D2: 4 more middleware end-to-end.
#
# Verifies:
#   - rewrite     : GET /old/data hits the /api/data handler
#   - static_files: GET /static/hello.txt returns the file content
#                   GET /static/../etc/passwd is rejected with 403
#   - gzip        : Accept-Encoding: gzip -> Content-Encoding: gzip,
#                   body decompresses back to the original
#   - error_pages : 404 / 500 bodies are swapped for the registered
#                   custom HTML
#
# Skips when curl(1) is missing.

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
        echo "  [SKIP-WIN] http_middleware_d2 — HTTP server/proxy/middleware code is platform-independent; covered by POSIX matrix"
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

# Pre-create the static directory inside TMPDIR. We pass the path to
# the server via STATIC_ROOT so it works cross-platform — on MSYS the
# native Windows binary doesn't understand /tmp/... paths, so we
# convert the MSYS path with cygpath when available.
STATIC_DIR="$TMPDIR/static-test"
mkdir -p "$STATIC_DIR"
echo "static-hello-content" > "$STATIC_DIR/hello.txt"
if command -v cygpath >/dev/null 2>&1; then
    STATIC_ROOT="$(cygpath -m "$STATIC_DIR")"
else
    STATIC_ROOT="$STATIC_DIR"
fi

AETHER_HOME="$ROOT" STATIC_ROOT="$STATIC_ROOT" \
    "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

URL_BASE="http://127.0.0.1:18106"

# Wait for the server to actually accept connections, not just print
# READY. server.ae prints READY before the SrvActor receives StartSrv
# and calls http_server_start_raw — on slow runners (Windows GHA)
# that gap can be ~hundreds of ms. Probe the port until any HTTP
# response comes back.
deadline=$(($(date +%s) + 15))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        echo "  [FAIL] server died:"
        head -30 "$TMPDIR/srv.log"
        exit 1
    fi
    if curl -s -o /dev/null --max-time 1 "$URL_BASE/" 2>/dev/null; then
        break
    fi
    sleep 0.1
done
if [ "$(date +%s)" -ge "$deadline" ]; then
    echo "  [FAIL] server never accepted connections within 15s"
    head -30 "$TMPDIR/srv.log"
    exit 1
fi

# --- rewrite: /old/data should hit the /api/data handler ---
status=$(curl -s -o "$TMPDIR/rw.body" -w '%{http_code}' --max-time 5 \
    "$URL_BASE/old/data")
if [ "$status" != "200" ]; then
    echo "  [FAIL] rewrite: /old/data expected 200; got $status"
    exit 1
fi
if ! grep -q "lorem ipsum" "$TMPDIR/rw.body"; then
    echo "  [FAIL] rewrite: body wrong; got: $(cat "$TMPDIR/rw.body")"
    exit 1
fi

# --- static_files: serve a file ---
status=$(curl -s -o "$TMPDIR/static.body" -w '%{http_code}' --max-time 5 \
    "$URL_BASE/static/hello.txt")
if [ "$status" != "200" ]; then
    echo "  [FAIL] static_files: /static/hello.txt expected 200; got $status"
    exit 1
fi
if ! grep -q "static-hello-content" "$TMPDIR/static.body"; then
    echo "  [FAIL] static_files: body wrong; got: $(cat "$TMPDIR/static.body")"
    exit 1
fi

# --- static_files: path traversal blocked ---
status=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
    "$URL_BASE/static/../etc/passwd")
# curl normalises the URL client-side. To actually send "../" we
# need --path-as-is.
status=$(curl -s -o /dev/null --path-as-is -w '%{http_code}' --max-time 5 \
    "$URL_BASE/static/../etc/passwd")
if [ "$status" != "403" ]; then
    echo "  [FAIL] static_files: traversal expected 403; got $status"
    exit 1
fi

# --- gzip: Accept-Encoding: gzip -> Content-Encoding: gzip ---
HEADERS="$TMPDIR/gz.h"
curl -s -D "$HEADERS" -o "$TMPDIR/gz.body" --max-time 5 \
    -H "Accept-Encoding: gzip" "$URL_BASE/api/data" --compressed
if ! grep -qi "Content-Encoding: gzip" "$HEADERS"; then
    echo "  [FAIL] gzip: missing Content-Encoding: gzip"
    cat "$HEADERS"
    exit 1
fi
if ! grep -qi "Vary: Accept-Encoding" "$HEADERS"; then
    echo "  [FAIL] gzip: missing Vary: Accept-Encoding"
    cat "$HEADERS"
    exit 1
fi
# curl --compressed transparently decompresses; body should be the
# original plaintext.
if ! grep -q "lorem ipsum" "$TMPDIR/gz.body"; then
    echo "  [FAIL] gzip: decompressed body wrong; got: $(cat "$TMPDIR/gz.body")"
    exit 1
fi

# --- gzip: client without Accept-Encoding gets uncompressed ---
HEADERS="$TMPDIR/gz_off.h"
curl -s -D "$HEADERS" -o /dev/null --max-time 5 "$URL_BASE/api/data"
if grep -qi "Content-Encoding: gzip" "$HEADERS"; then
    echo "  [FAIL] gzip: compressed even when client did not accept gzip"
    cat "$HEADERS"
    exit 1
fi

# --- error_pages: 500 swapped for custom HTML ---
curl -s -o "$TMPDIR/500.body" --max-time 5 "$URL_BASE/kaboom" >/dev/null
if ! grep -q "Custom Internal Error" "$TMPDIR/500.body"; then
    echo "  [FAIL] error_pages: 500 body not swapped; got: $(cat "$TMPDIR/500.body")"
    exit 1
fi

# --- error_pages: 404 swapped for custom HTML ---
curl -s -o "$TMPDIR/404.body" --max-time 5 "$URL_BASE/no-such-route" >/dev/null
if ! grep -q "Custom Not Found" "$TMPDIR/404.body"; then
    echo "  [FAIL] error_pages: 404 body not swapped; got: $(cat "$TMPDIR/404.body")"
    exit 1
fi

echo "  [PASS] http_middleware_d2: rewrite / static_files / gzip / error_pages all enforced"
