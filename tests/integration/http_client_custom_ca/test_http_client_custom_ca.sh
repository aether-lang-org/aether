#!/bin/sh
# Regression: #1107 — std.http.client per-request custom CA pin (set_cafile).
#
# Starts an Aether TLS server with a self-signed cert (SAN IP:127.0.0.1), then
# runs an Aether client that hits it three ways: pin the correct CA (must
# verify -> 200), pin a WRONG CA (must fail closed), and no cafile (system store
# must reject the self-signed). This is the "verify, but against THIS cert"
# path — strictly stronger than set_insecure. See client.ae.
#
# Skips cleanly when openssl is missing or the build has no OpenSSL.

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_client_custom_ca — TLS code is platform-independent; covered by POSIX matrix"
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

TMPDIR="$(mktemp -d)"
cleanup() {
    if [ -n "${SRV_PID:-}" ]; then
        kill "$SRV_PID" 2>/dev/null || true
        wait "$SRV_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# Unique port 18119 (18118 is http_client_insecure_tls). `ae run` forks the
# server child, so the trap can't fully reap it; a distinct port keeps a
# lingering server from starving the next test.

CERT="$TMPDIR/cert.pem"      # server's own cert == the CA we pin (GOOD_CA)
KEY="$TMPDIR/key.pem"
BADCA="$TMPDIR/badca.pem"    # an unrelated self-signed cert (WRONG CA)

# Server cert with SAN IP:127.0.0.1, so the pinning client passes hostname
# verification connecting by IP (peer + hostname verification stay ON with
# set_cafile — that's the whole point vs set_insecure).
if ! openssl req -x509 -newkey rsa:2048 \
        -keyout "$KEY" -out "$CERT" -days 1 -nodes \
        -subj "/CN=127.0.0.1" \
        -addext "subjectAltName=IP:127.0.0.1" 2>"$TMPDIR/openssl.err"; then
    echo "  [SKIP] openssl req (server cert) failed:"
    head -5 "$TMPDIR/openssl.err"
    exit 0
fi

# A DIFFERENT self-signed cert — the "wrong CA" the client pins to prove the
# pin is enforced (this CA does not sign the server's cert).
if ! openssl req -x509 -newkey rsa:2048 \
        -keyout "$TMPDIR/badkey.pem" -out "$BADCA" -days 1 -nodes \
        -subj "/CN=wrong-ca" 2>"$TMPDIR/openssl2.err"; then
    echo "  [SKIP] openssl req (bad CA) failed:"
    head -5 "$TMPDIR/openssl2.err"
    exit 0
fi

AETHER_HOME="$ROOT" CERT_PATH="$CERT" KEY_PATH="$KEY" \
    "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

deadline=$(($(date +%s) + 30))
while [ "$(date +%s)" -lt "$deadline" ]; do
    grep -q READY "$TMPDIR/srv.log" 2>/dev/null && break
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        if grep -q "TLS unavailable" "$TMPDIR/srv.log" 2>/dev/null; then
            echo "  [SKIP] build has no OpenSSL"
            exit 0
        fi
        echo "  [FAIL] server died before READY:"
        head -20 "$TMPDIR/srv.log"
        exit 1
    fi
    sleep 0.1
done
if ! grep -q READY "$TMPDIR/srv.log" 2>/dev/null; then
    if grep -q "TLS unavailable" "$TMPDIR/srv.log" 2>/dev/null; then
        echo "  [SKIP] build has no OpenSSL"
        exit 0
    fi
    echo "  [FAIL] server never reported READY:"
    head -20 "$TMPDIR/srv.log"
    exit 1
fi

sleep 0.3

OUT="$TMPDIR/client.out"
if ! AETHER_HOME="$ROOT" GOOD_CA="$CERT" BAD_CA="$BADCA" \
        "$AE" run "$SCRIPT_DIR/client.ae" >"$OUT" 2>&1; then
    echo "  [FAIL] client exited non-zero:"
    head -10 "$OUT"
    exit 1
fi

if grep -q '^PASS ' "$OUT"; then
    echo "  [PASS] http_client_custom_ca: correct CA verifies, wrong CA fails closed, default rejects"
    exit 0
fi

echo "  [FAIL] client did not report PASS:"
head -10 "$OUT"
exit 1
