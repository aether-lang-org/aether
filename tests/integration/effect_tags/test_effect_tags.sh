#!/bin/sh
# Issue #481: a function carrying an effect tag (@pure / @no_fs / @no_net /
# @no_os) must not transitively reach the forbidden capability. Each reject_*.ae
# below violates its tag and MUST fail to build with the effect diagnostic.
# Allowed shapes are covered by test_issue481_effect_tags.ae. Pruned from the
# generic .ae runner so this driver owns the negative cells.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
if [ ! -x "$AE" ]; then echo "  [SKIP] effect_tags: $AE not built"; exit 0; fi
TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

check_rejected() {
    name="$1"; cap="$2"
    if "$AE" build "$SCRIPT_DIR/$name.ae" -o "$TMPDIR/out" >"$TMPDIR/$name.log" 2>&1; then
        echo "  [FAIL] effect_tags: $name.ae compiled (should be rejected)"; exit 1
    fi
    if ! grep -q "reaches a \`$cap\` operation" "$TMPDIR/$name.log"; then
        echo "  [FAIL] effect_tags: $name.ae failed without the $cap effect diagnostic"
        sed 's/^/    /' "$TMPDIR/$name.log" | head -10; exit 1
    fi
}

check_rejected reject_fs fs
check_rejected reject_transitive os
check_rejected reject_net net

echo "  [PASS] effect_tags: @no_fs / @pure-transitive / @no_net violations all rejected"
exit 0
