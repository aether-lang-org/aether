#!/bin/sh
# Integration test for the selective-import-shadowing diagnostic
# (issue #436 facet A). Three cases:
#
#   1. Module with shadow → build rejected, diagnostic mentions the
#      colliding local def + the import it shadows + a precise fix.
#   2. Companion module without shadow → builds + runs cleanly.
#   3. Same shadow inside MAIN.ae (not a module) → rejected by the
#      orchestrator-level check on the entry-point AST.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

pass=0; fail=0

# Clear the build cache so we don't pick up a stale binary from a
# pre-fix run.
"$AE" cache clear >/dev/null 2>&1 || true

# Case 1: module shadow → build error with the diagnostic text.
out=$("$AE" build "$SCRIPT_DIR/main_uses_shadow.ae" \
      --lib "$SCRIPT_DIR" -o "$TMPDIR/uses_shadow" 2>&1)
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "  [FAIL] case 1: expected build to fail, but it succeeded"
    fail=$((fail + 1))
elif printf '%s\n' "$out" | grep -q "error\[E1000\]" && \
     printf '%s\n' "$out" | grep -q "selectively imports 'length'"; then
    echo "  [PASS] case 1: module shadow rejected with E1000 diagnostic"
    pass=$((pass + 1))
else
    echo "  [FAIL] case 1: build failed but without the expected diagnostic"
    echo "$out"
    fail=$((fail + 1))
fi

# Case 2: clean module → build + run cleanly.
if ! "$AE" build "$SCRIPT_DIR/main_uses_clean.ae" \
     --lib "$SCRIPT_DIR" -o "$TMPDIR/uses_clean" >"$TMPDIR/clean.log" 2>&1; then
    echo "  [FAIL] case 2: expected clean build, got error"
    cat "$TMPDIR/clean.log"
    fail=$((fail + 1))
else
    out=$("$TMPDIR/uses_clean")
    if [ "$out" = "n=5" ]; then
        echo "  [PASS] case 2: clean (rename-the-local) module builds + runs"
        pass=$((pass + 1))
    else
        echo "  [FAIL] case 2: ran but produced '$out', expected 'n=5'"
        fail=$((fail + 1))
    fi
fi

# Case 3: same shadow but inline in main.ae itself.
out=$("$AE" build "$SCRIPT_DIR/main_inline_shadow.ae" \
      -o "$TMPDIR/inline_shadow" 2>&1)
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "  [FAIL] case 3: expected build to fail, but it succeeded"
    fail=$((fail + 1))
elif printf '%s\n' "$out" | grep -q "error\[E1000\]" && \
     printf '%s\n' "$out" | grep -q "main program"; then
    echo "  [PASS] case 3: inline-in-main shadow rejected with E1000 diagnostic"
    pass=$((pass + 1))
else
    echo "  [FAIL] case 3: build failed but without the expected diagnostic"
    echo "$out"
    fail=$((fail + 1))
fi

echo
echo "selective_import_shadow: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
