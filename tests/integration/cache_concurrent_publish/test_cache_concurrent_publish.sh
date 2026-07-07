#!/bin/sh
# Regression for #1032 (half 2): concurrent same-key `ae run`s must never
# exec a partially-written cache slot.
#
# Before the fix, `ae run` pointed the LINKER at the final cache slot and
# the hit path was `path_exists -> exec`, so a second invocation landing
# mid-link exec'd a truncated binary (ENOEXEC or a garbage segfault). Now
# writers link to <slot>.tmp.<pid> and publish with an atomic rename, so
# every reader sees a complete file or a miss.
#
# This hammers N parallel runs of the same fresh source (shared key, cold
# cache) and requires every one to exit 0 with the right output.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] $AE not built (run make first)"
    exit 0
fi

TMPDIR_T="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_T"' EXIT

CACHE="$TMPDIR_T/cache"   # private cache => guaranteed cold + no pollution
TOKEN="ccp-$$-$(date +%s)"
cat > "$TMPDIR_T/prog.ae" <<AE
extern println(s: string)
main() { println("$TOKEN") }
AE

N=8
i=1
while [ "$i" -le "$N" ]; do
    ( AETHER_CACHE_DIR="$CACHE" "$AE" run "$TMPDIR_T/prog.ae" \
        > "$TMPDIR_T/out.$i" 2>&1; echo "$?" > "$TMPDIR_T/rc.$i" ) &
    i=$((i + 1))
done
wait

fails=0
i=1
while [ "$i" -le "$N" ]; do
    rc=$(cat "$TMPDIR_T/rc.$i" 2>/dev/null || echo 99)
    if [ "$rc" -ne 0 ]; then
        echo "  [FAIL] parallel run $i exited $rc:"
        sed 's/^/        /' "$TMPDIR_T/out.$i"
        fails=$((fails + 1))
    elif ! grep -q "$TOKEN" "$TMPDIR_T/out.$i"; then
        echo "  [FAIL] parallel run $i produced wrong output:"
        sed 's/^/        /' "$TMPDIR_T/out.$i"
        fails=$((fails + 1))
    fi
    i=$((i + 1))
done

# No temp slots may survive an orderly finish (each writer publishes or
# cleans up its own .tmp.<pid>).
leftover=$(ls "$CACHE" 2>/dev/null | grep -c '\.tmp\.' || true)
if [ "$leftover" -ne 0 ]; then
    echo "  [FAIL] $leftover orphaned .tmp.* slots left in cache:"
    ls "$CACHE" | sed 's/^/        /'
    fails=$((fails + 1))
fi

if [ "$fails" -ne 0 ]; then
    exit 1
fi
echo "  [PASS] $N concurrent same-key runs: all exited 0, no partial slots, no orphans"
exit 0
