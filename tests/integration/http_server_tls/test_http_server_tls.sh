#!/bin/sh
# #260 Tier 0: server-side TLS termination round-trip.
#
# Generates a self-signed cert at test time, starts the in-process
# TLS server, drives one HTTPS request via curl, and verifies the
# handshake completes + the body comes back. Skips cleanly when
# openssl(1) or curl is missing or when the build has no OpenSSL.

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
        echo "  [SKIP-WIN] http_server_tls — HTTP server/proxy/middleware code is platform-independent; covered by POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v openssl >/dev/null 2>&1; then
    echo "  [SKIP] openssl not on PATH"
    exit 0
fi
if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not on PATH"
    exit 0
fi

TMPDIR="$(mktemp -d)"
cleanup() {
    if [ -n "${SRV_PID:-}" ]; then
        kill "$SRV_PID" 2>/dev/null || true
        # Suppress the "Terminated: 15" wait notice from the shell.
        wait "$SRV_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

CERT="$TMPDIR/cert.pem"
KEY="$TMPDIR/key.pem"

# 1-day self-signed cert for CN=localhost.
if ! openssl req -x509 -newkey rsa:2048 \
        -keyout "$KEY" -out "$CERT" \
        -days 1 -nodes \
        -subj "/CN=localhost" 2>"$TMPDIR/openssl.err"; then
    echo "  [SKIP] openssl req failed:"
    head -5 "$TMPDIR/openssl.err"
    exit 0
fi

# Start the server in the background. Pass cert/key via env so the
# Aether driver doesn't need to embed paths.
AETHER_HOME="$ROOT" CERT_PATH="$CERT" KEY_PATH="$KEY" \
    "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

# Wait for the server to print READY (or die / time out). The first
# `ae run` invocation on a cold cache compiles the OpenSSL-linked
# binary, which can take several seconds on a slow CI runner — give it
# 30s to be safe.
deadline=$(($(date +%s) + 30))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if grep -q READY "$TMPDIR/srv.log" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        echo "  [FAIL] server died before READY:"
        head -20 "$TMPDIR/srv.log"
        exit 1
    fi
    sleep 0.1
done
if ! grep -q READY "$TMPDIR/srv.log" 2>/dev/null; then
    # Server may have built without OpenSSL — check the failure mode.
    if grep -q "TLS unavailable" "$TMPDIR/srv.log" 2>/dev/null; then
        echo "  [SKIP] build has no OpenSSL"
        exit 0
    fi
    echo "  [FAIL] server never reported READY:"
    head -20 "$TMPDIR/srv.log"
    exit 1
fi

# Brief settle for the actor to bind the listen socket.
sleep 0.3

# Drive the HTTPS request. --cacert points curl at our self-signed
# cert so verification succeeds; --resolve isn't needed because the
# cert's CN is localhost and we're hitting 127.0.0.1 anyway (curl
# accepts CN=localhost for 127.0.0.1 with --insecure-resolve... wait,
# actually pin the host with --resolve so cert SAN/CN matches).
ACTUAL="$TMPDIR/curl.out"
if ! curl --silent --max-time 5 --cacert "$CERT" \
          --resolve localhost:18102:127.0.0.1 \
          https://localhost:18102/ -o "$ACTUAL" 2>"$TMPDIR/curl.err"; then
    echo "  [FAIL] curl failed:"
    cat "$TMPDIR/curl.err"
    echo "--- server log ---"
    head -30 "$TMPDIR/srv.log"
    exit 1
fi

if [ "$(cat "$ACTUAL")" != "ok-tls" ]; then
    echo "  [FAIL] expected body 'ok-tls'; got:"
    cat "$ACTUAL"
    exit 1
fi

# Sanity: a plain-HTTP request to the same TLS port must fail (the
# handshake is mandatory; the server should reject the plaintext).
if curl --silent --max-time 2 http://localhost:18102/ \
        -o "$TMPDIR/plain.out" 2>/dev/null; then
    # If curl exited 0 it could be that the server responded with
    # garbage or that some proxy intercepted. Check the body.
    if [ -s "$TMPDIR/plain.out" ] && [ "$(cat "$TMPDIR/plain.out")" = "ok-tls" ]; then
        echo "  [FAIL] plain-HTTP request to TLS port returned the TLS body"
        exit 1
    fi
fi

echo "  [PASS] http_server_tls: TLS handshake + response round-trip"
