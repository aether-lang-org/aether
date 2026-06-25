#!/bin/sh
# Issue #564 + #790: heap.new(T) / heap.free(p) — heap box.
#   probe.ae          must build + run, printing the PASS line.
#   accept_string.ae  must build + run (#790: string-field struct is owned).
#   reject_nonstruct.ae must FAIL to build (T is not a struct).
# The directory is pruned from the generic .ae runner (Makefile) so this
# driver owns the cells.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

# ---- positive: build + run ----
if ! "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] heap_new_pod: probe build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -20
    exit 1
fi
if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] heap_new_pod: probe exited non-zero"
    sed 's/^/    /' "$TMPDIR/run.log" | head -20
    exit 1
fi
if ! grep -q "PASS: heap.new POD success path" "$TMPDIR/run.log"; then
    echo "  [FAIL] heap_new_pod: probe didn't reach PASS"
    sed 's/^/    /' "$TMPDIR/run.log" | head -20
    exit 1
fi

# ---- positive: string-field struct accepted, owned, and freed (#790) ----
if ! "$AE" build "$SCRIPT_DIR/accept_string.ae" -o "$TMPDIR/as" \
        >"$TMPDIR/as.log" 2>&1; then
    echo "  [FAIL] heap_new_pod: accept_string.ae failed to build (#790: should be accepted)"
    sed 's/^/    /' "$TMPDIR/as.log" | head -20
    exit 1
fi
if ! "$TMPDIR/as" >"$TMPDIR/as_run.log" 2>&1 || \
   ! grep -q "PASS: heap.new string-field box" "$TMPDIR/as_run.log"; then
    echo "  [FAIL] heap_new_pod: accept_string.ae didn't reach PASS"
    sed 's/^/    /' "$TMPDIR/as_run.log" | head -20
    exit 1
fi

# ---- negative: non-struct type rejected ----
if "$AE" build "$SCRIPT_DIR/reject_nonstruct.ae" -o "$TMPDIR/rn" \
        >"$TMPDIR/rn.log" 2>&1; then
    echo "  [FAIL] heap_new_pod: reject_nonstruct.ae compiled (should be rejected)"
    exit 1
fi
if ! grep -q "is not a struct type" "$TMPDIR/rn.log"; then
    echo "  [FAIL] heap_new_pod: reject_nonstruct.ae failed but without the not-a-struct diagnostic"
    sed 's/^/    /' "$TMPDIR/rn.log" | head -10
    exit 1
fi

echo "  [PASS] heap_new_pod: POD box works; string-field box owned (#790); non-struct rejected"
exit 0
