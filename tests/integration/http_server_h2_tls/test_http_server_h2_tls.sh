#!/bin/sh
# #260 Tier 2: HTTP/2 over TLS, ALPN-negotiated.
#
# Generates a self-signed cert, starts the in-process h2+TLS
# server, drives one HTTPS request via curl --http2, and verifies
# the handshake completes + ALPN negotiated h2 + the body comes
# back. Skips cleanly when openssl(1), curl, libnghttp2, or
# OpenSSL is missing.

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
        echo "  [SKIP-WIN] http_server_h2_tls — HTTP server/proxy/middleware code is platform-independent; covered by POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v openssl >/dev/null 2>&1; then
    echo "  [SKIP] openssl not on PATH"; exit 0
fi
if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not on PATH"; exit 0
fi
if ! curl --version 2>&1 | grep -q nghttp2; then
    echo "  [SKIP] curl built without HTTP/2"; exit 0
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

CERT="$TMPDIR/cert.pem"
KEY="$TMPDIR/key.pem"

if ! openssl req -x509 -newkey rsa:2048 \
        -keyout "$KEY" -out "$CERT" \
        -days 1 -nodes \
        -subj "/CN=localhost" 2>"$TMPDIR/openssl.err"; then
    echo "  [SKIP] openssl req failed:"
    head -5 "$TMPDIR/openssl.err"
    exit 0
fi

AETHER_HOME="$ROOT" CERT_PATH="$CERT" KEY_PATH="$KEY" \
    "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

deadline=$(($(date +%s) + 30))
ready=""
while [ "$(date +%s)" -lt "$deadline" ]; do
    if grep -q '^READY' "$TMPDIR/srv.log" 2>/dev/null; then
        ready=$(grep '^READY' "$TMPDIR/srv.log" | head -1); break
    fi
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        echo "  [FAIL] server died:"; head -20 "$TMPDIR/srv.log"; exit 1
    fi
    sleep 0.1
done
[ -z "$ready" ] && {
    echo "  [FAIL] no READY within timeout"; head -20 "$TMPDIR/srv.log"; exit 1
}
case "$ready" in READY-NOH2*) echo "  [SKIP] $ready"; exit 0;; esac
sleep 0.3

URL="https://127.0.0.1:18263/"
RESP="$TMPDIR/resp"
# --insecure: self-signed cert. --http2: ask curl to negotiate
# HTTP/2 via ALPN. -w prints the negotiated http_version.
HTTPVER=$(curl --silent --show-error --max-time 5 \
              --insecure \
              --http2 \
              -o "$RESP" \
              -w '%{http_version}' \
              "$URL" 2>"$TMPDIR/curl.err") || {
    echo "  [FAIL] curl --http2 over TLS failed:"; cat "$TMPDIR/curl.err"; exit 1
}

if [ "$HTTPVER" != "2" ]; then
    echo "  [FAIL] ALPN didn't negotiate h2 — got http_version='$HTTPVER'"
    cat "$TMPDIR/curl.err"
    exit 1
fi

if ! grep -q '^h2-tls-ok$' "$RESP"; then
    echo "  [FAIL] h2-over-TLS body mismatch:"; cat "$RESP"; exit 1
fi

# Sanity: an HTTP/1.1 client (curl --http1.1) must STILL work
# against the same server. ALPN advertises both protocols; the
# client side decides.
RESP11="$TMPDIR/resp11"
HTTPVER11=$(curl --silent --show-error --max-time 5 \
                --insecure \
                --http1.1 \
                -o "$RESP11" \
                -w '%{http_version}' \
                "$URL" 2>"$TMPDIR/curl11.err") || {
    echo "  [FAIL] HTTP/1.1 fallback failed:"; cat "$TMPDIR/curl11.err"; exit 1
}
if [ "$HTTPVER11" != "1.1" ]; then
    echo "  [FAIL] HTTP/1.1 fallback didn't pick 1.1 — got '$HTTPVER11'"
    exit 1
fi
if ! grep -q '^h2-tls-ok$' "$RESP11"; then
    echo "  [FAIL] HTTP/1.1 fallback body mismatch:"; cat "$RESP11"; exit 1
fi

echo "  [PASS] HTTP/2 over TLS (ALPN-negotiated) (issue #260 Tier 2)"
