#!/bin/sh
# Serve-and-dial: an in-handler HTTP client call round-trips a body back out.
#
# Self-contained — the .ae binary hosts both servers and drives the request
# itself, so no curl / port dance. Asserts the program prints
# SERVE_AND_DIAL_OK, which requires:
#   - non-blocking server start (both servers bind in one process), and
#   - thread-safe name resolution (inner dial on a worker thread does not
#     corrupt main's dial via a shared gethostbyname buffer).

# Skip on Windows — the HTTP server/client code path under test is
# platform-independent userland C, already exercised by the POSIX matrix;
# the localhost + fork overhead under MSYS2 adds minutes without coverage.
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_serve_and_dial — HTTP code is platform-independent; covered by POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

OUT="$("$AE" run "$SCRIPT_DIR/server.ae" 2>&1)" || {
    echo "  FAIL: program exited non-zero"
    echo "$OUT"
    exit 1
}

if printf '%s\n' "$OUT" | grep -q '^SERVE_AND_DIAL_OK$'; then
    echo "  PASS: serve-and-dial body round-tripped"
    exit 0
fi

echo "  FAIL: expected SERVE_AND_DIAL_OK"
echo "$OUT"
exit 1
