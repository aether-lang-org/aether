#!/bin/sh
# Privacy invariant guard for `ae help` (issue #414).
#
# The command's documented contract: NO network calls, ever.
# Verified at runtime by running ae help under a syscall tracer
# that captures every socket-creating syscall and asserting the
# count is zero.
#
# Strategy:
#   - Linux: strace -e trace=network. Counts socket(2), connect(2),
#     sendto(2), recvfrom(2), socketpair(2), bind(2), accept(2),
#     listen(2). Zero is the only acceptable result.
#   - macOS / other: no portable equivalent without sudo. The test
#     skips with a clear status message (still exits 0); the Linux
#     CI matrix entry is the canonical guard.
#
# The fixture is intentionally a script that triggers ALL
# heuristic capabilities (Levenshtein, YAML, missing-import,
# top-level DSL) so the tracer covers every code path.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

cat > "$TMPDIR/many_findings.ae" <<EOF
import std.bytes (length)
import my_lib

my_lib.serve {
    port: 9990
    host = "127.0.0.1"
}

main() {
    s = "hello"
    n = lenght(s)
    println("\${n}")
}
EOF

case "$(uname -s)" in
    Linux*)
        if ! command -v strace >/dev/null 2>&1; then
            echo "  [SKIP] strace not available on this Linux host"
            echo "ae_help_privacy: 0 passed, 0 failed (skipped)"
            exit 0
        fi
        strace_out="$TMPDIR/strace.out"
        # -f follow children (aetherc subprocess). Network filter
        # captures every socket-touching syscall.
        strace -f -e trace=network -o "$strace_out" \
            "$AE" help "$TMPDIR/many_findings.ae" > "$TMPDIR/stdout" 2>&1 || true
        # Count network syscalls. `strace` prints one per call; any
        # of these substrings means we made a call we shouldn't.
        net_calls=$(grep -cE 'socket\(|connect\(|sendto\(|recvfrom\(|socketpair\(|bind\(|listen\(|accept\(' "$strace_out" 2>/dev/null || true)
        net_calls=${net_calls:-0}
        if [ "$net_calls" -eq 0 ]; then
            echo "  [PASS] zero network syscalls during ae help"
            echo "ae_help_privacy: 1 passed, 0 failed"
            exit 0
        else
            echo "  [FAIL] ae help made $net_calls network syscall(s)"
            grep -E 'socket\(|connect\(|sendto\(|recvfrom\(|socketpair\(|bind\(|listen\(|accept\(' "$strace_out" | head -20
            exit 1
        fi
        ;;
    *)
        echo "  [SKIP] non-Linux platform; Linux strace matrix is the canonical guard"
        echo "ae_help_privacy: 0 passed, 0 failed (skipped)"
        exit 0
        ;;
esac
