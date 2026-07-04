#!/bin/sh
# Regression: aether#1012 part 1 — std.http.client per-request TLS-verify-skip.
#
# Starts an in-process Aether TLS server with a self-signed cert, then runs an
# Aether client that hits it twice: default (must reject the cert) and
# client.set_insecure(req, 1) (must succeed). See client.ae for the assertion.
#
# Skips cleanly when openssl is missing or the build has no OpenSSL.

# Skip on Windows — TLS client/server code is platform-independent userland C
# already covered by the POSIX matrix; MSYS2 openssl + fork overhead adds
# minutes without coverage.
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_client_insecure_tls — TLS code is platform-independent; covered by POSIX matrix"
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
        # Suppress the "Terminated: 15" wait notice from the shell.
        wait "$SRV_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# NOTE: this test binds a UNIQUE port (18118) that no other integration test
# uses. `ae run` forks the compiled server as a child, so the trap above can't
# fully reap it (the fork outlives the wrapper) — every server test in this
# suite has that limitation. A unique port is what keeps a lingering server
# from starving the next test (the collision that surfaced this on macOS, where
# reaping is slower). Keep 18118 distinct if you add a sibling test.

CERT="$TMPDIR/cert.pem"
KEY="$TMPDIR/key.pem"

# 1-day self-signed cert for CN=localhost.
if ! openssl req -x509 -newkey rsa:2048 \
        -keyout "$KEY" -out "$CERT" -days 1 -nodes \
        -subj "/CN=localhost" 2>"$TMPDIR/openssl.err"; then
    echo "  [SKIP] openssl req failed:"
    head -5 "$TMPDIR/openssl.err"
    exit 0
fi

# Start the self-signed TLS server.
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

# Settle for the actor to bind the listen socket.
sleep 0.3

OUT="$TMPDIR/client.out"
if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/client.ae" >"$OUT" 2>&1; then
    echo "  [FAIL] client exited non-zero:"
    cat "$OUT" | head -10
    exit 1
fi

if grep -q '^PASS ' "$OUT"; then
    echo "  [PASS] http_client_insecure_tls: default rejects self-signed, set_insecure accepts"
    exit 0
fi

echo "  [FAIL] client did not report PASS:"
cat "$OUT" | head -10
exit 1
