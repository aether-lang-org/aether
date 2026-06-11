#!/bin/sh
# Integration test: `ae run <file.ae> -- <args>` forwards everything after
# the `--` separator to the running program's argv (like `cargo run --`).
# A config-is-code entry point (e.g. an Aether build supervisor) needs
# this so `ae run supervisor.ae -- make -j8` sees make/-j8 in its args.
#
# Asserts:
#   1. Args after `--` reach the program (they were dropped before).
#   2. A single arg containing spaces stays ONE token (double-quoted
#      through run_cmd's tokenizer), not split.
#   3. No `--` → no forwarded args (argc == 1).

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
MAIN="$SCRIPT_DIR/main.ae"

fail() { echo "  FAIL: $1"; exit 1; }

# --- Case 1 + 2: forward three args, the middle one with a space -------
out=$("$AE" run "$MAIN" -- alpha "beta gamma" delta 2>&1)

echo "$out" | grep -q "argc=4" || fail "expected argc=4, got: $(echo "$out" | grep argc)"
echo "$out" | grep -qx "ARG:alpha"      || fail "missing ARG:alpha"
echo "$out" | grep -qx "ARG:beta gamma" || fail "spaces-arg was split or lost (expected 'ARG:beta gamma')"
echo "$out" | grep -qx "ARG:delta"      || fail "missing ARG:delta"

# --- Case 3: no `--` → nothing forwarded -------------------------------
out2=$("$AE" run "$MAIN" 2>&1)
echo "$out2" | grep -q "argc=1" || fail "expected argc=1 with no '--', got: $(echo "$out2" | grep argc)"

echo "  PASS: ae run forwards post-'--' args (spaces preserved); none without '--'"
