#!/bin/sh
# tests/run_macos_leaks.sh — macOS leaks(1) gate for the memory-
# ownership regression suite (#468).
#
# Why this exists: macOS has no working leak sanitizer. Apple's
# bundled clang ships ASan WITHOUT LeakSanitizer, and upstream LLVM's
# LSan hangs on Apple Silicon (sanitizers#1026, LLVM discourse 73148).
# Linux CI gets leak coverage from Valgrind (`make test-valgrind`) and
# ASan-with-LSan; macOS got nothing — which is exactly how the #471
# CI failures slipped past local macOS runs. Apple's `leaks(1)` tool
# is the one detector that works here: it inspects the heap at exit
# with no rebuild and no sanitizer flag.
#
# Scope: a CURATED set of memory-exercising regression programs — the
# return-escape / struct-field / actor-message / list-map ownership
# tests from #459/#465/#466/#467, plus the collections edge-case
# probes. This is deliberately NOT every integration test:
# MallocStackLogging adds ~10x runtime, so gating the whole suite
# would balloon CI wall-clock for little marginal coverage (the
# ownership machinery is what regresses; plain logic tests don't
# leak). The covered set is printed below so the subset choice is
# visible, never mistaken for "all tests".
#
# Each program is built with `ae build` (release -O2) and run under
# `MallocStackLogging=1 leaks --atExit`. Non-zero leak count on any
# program fails the gate.
#
# Skips cleanly (exit 0) on non-Darwin hosts.

set -e

if [ "$(uname -s)" != "Darwin" ]; then
    echo "[SKIP] test-macos-leaks: leaks(1) is macOS-only (Linux uses Valgrind/ASan)"
    exit 0
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
AE="$ROOT/build/ae"
REG="$ROOT/tests/regression"

if [ ! -x "$AE" ]; then
    echo "[SKIP] test-macos-leaks: build/ae not built (run: make ae)"
    exit 0
fi

# Curated memory-exercising programs (leak-detector gate).
# Add new ownership/allocation regressions here when they land.
TESTS="
test_return_escape_bare_local
test_return_escape_loop
test_return_escape_mixed
test_return_escape_recursive
test_return_escape_user_chain
test_return_escape_dup_arg
test_return_escape_return_in_loop
test_return_escape_self_reassign
test_struct_field_heap_tracking
test_struct_field_heap_multi
test_actor_message_heap_string
test_list_heap_string_ownership
test_map_heap_string_value
test_collections_edge_cases
test_collections_hashmap_edge
test_collections_wrappers
"

echo "=================================================="
echo "macOS leaks(1) gate — curated memory-ownership set"
echo "=================================================="

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# Per-step timeout with a PROCESS-GROUP kill so a stuck step can't hang
# the whole CI job until the 6-hour ceiling. This must kill the group,
# not just the direct child: `leaks --atExit -- prog` forks `prog`, and a
# plain `timeout`/`gtimeout` (or single-pid kill) only kills `leaks` —
# leaving `prog` orphaned and still holding the captured output open, so
# the caller hangs anyway. We fork the command into its OWN process group
# and, on timeout, `kill -KILL` the whole group (-pid), taking `leaks`
# AND `prog` down together. perl ships on every macOS runner. Exit code
# 124 == timed out.
run_bounded() {
    _secs="$1"; shift
    perl -e '
        use POSIX ();
        my $s = shift @ARGV;
        my $pid = fork();
        if (!defined $pid) { exit 127; }
        if ($pid == 0) { POSIX::setpgid(0, 0); exec(@ARGV); exit 127; }
        my $timed_out = 0;
        local $SIG{ALRM} = sub { $timed_out = 1; kill("KILL", -$pid); };
        alarm($s);
        waitpid($pid, 0);
        my $rc = $?;
        alarm(0);
        exit(124) if $timed_out;
        exit( ($rc & 127) ? (128 + ($rc & 127)) : ($rc >> 8) );
    ' "$_secs" "$@"
}

# Build timeout is generous (cold stdlib relink on a loaded runner);
# leaks(1) + MallocStackLogging runs ~10x slower than the bare program.
BUILD_TIMEOUT=240
LEAKS_TIMEOUT=120

fails=0
covered=0
for t in $TESTS; do
    src="$REG/$t.ae"
    if [ ! -f "$src" ]; then
        echo "  [MISS] $t.ae not found — skipping"
        continue
    fi
    bin="$tmpdir/$t"
    # Newline-terminated progress line (NOT \r) so it flushes and is
    # visible in the non-TTY CI log — the in-flight test is always named,
    # so a slow/stuck step is obvious from the live output.
    echo "  [ .. ] $t: build + leak-check ..."
    run_bounded "$BUILD_TIMEOUT" "$AE" build "$src" -o "$bin" >"$tmpdir/build.log" 2>&1
    brc=$?
    if [ "$brc" -eq 124 ]; then
        echo "  [FAIL] $t: build TIMED OUT (>${BUILD_TIMEOUT}s)"
        fails=$((fails + 1))
        continue
    elif [ "$brc" -ne 0 ]; then
        echo "  [FAIL] $t: build failed"
        sed 's/^/      /' "$tmpdir/build.log" | tail -10
        fails=$((fails + 1))
        continue
    fi
    # Capture via a FILE, not a `$(...)` pipe: with a pipe, a leaks(1) run
    # whose target program doesn't exit would orphan the program holding
    # the pipe's write end, and the command substitution would block even
    # after run_bounded kills the process group. A file has no such reader
    # dependency — the shell waits only for run_bounded.
    lk_out="$tmpdir/leaks_$t.out"
    MallocStackLogging=1 run_bounded "$LEAKS_TIMEOUT" leaks --atExit -- "$bin" >"$lk_out" 2>&1
    lrc=$?
    covered=$((covered + 1))
    if [ "$lrc" -eq 124 ]; then
        # `leaks` runs the target in its own process group, so the
        # run_bounded group-kill above won't reach an orphaned target —
        # reap it by path so a stuck program can't accumulate across the
        # run. (Harmless if already gone.)
        pkill -9 -f "$bin" 2>/dev/null || true
        echo "  [FAIL] $t: leaks(1) TIMED OUT (>${LEAKS_TIMEOUT}s) — program did not exit"
        fails=$((fails + 1))
        continue
    fi
    # `leaks` prints "Process N: K leaks for B total leaked bytes."
    count="$(sed -nE 's/.*: ([0-9]+) leaks for [0-9]+ total.*/\1/p' "$lk_out" | head -1)"
    if [ "$count" = "0" ]; then
        echo "  [PASS] $t: 0 leaks"
    else
        echo "  [FAIL] $t: ${count:-?} leaks"
        grep -E "ROOT LEAK|leaks for" "$lk_out" | head -12 | sed 's/^/      /'
        fails=$((fails + 1))
    fi
done

echo "--------------------------------------------------"
echo "covered $covered programs, $fails leaking"
if [ "$fails" -ne 0 ]; then
    echo "FAIL: macOS leaks gate found leaks"
    exit 1
fi
echo "PASS: macOS leaks gate clean"
