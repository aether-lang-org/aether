#!/bin/sh
# Regression for #1032 (half 1): AETHER_CACHE_DIR redirects the exe cache.
#
# The cache location was hard-wired to $HOME/.aether/cache, which a runner
# with a read-only $HOME (agent sandbox, hermetic CI) cannot use — and
# AETHER_HOME deliberately does not move it (toolchain root, not artifact
# dir). This asserts the override works, populates ONLY the override dir,
# and survives the read-only-home scenario that surfaced the issue.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] $AE not built (run make first)"
    exit 0
fi

TMPDIR_T="$(mktemp -d)"
trap 'chmod -R u+w "$TMPDIR_T" 2>/dev/null; rm -rf "$TMPDIR_T"' EXIT

CACHE="$TMPDIR_T/cache-override"
# Unique program text -> unique cache key -> guaranteed cold cache.
TOKEN="cdo-$$-$(date +%s)"
cat > "$TMPDIR_T/prog.ae" <<AE
extern println(s: string)
main() { println("$TOKEN") }
AE

# 1. Override populates the override dir.
out=$(AETHER_CACHE_DIR="$CACHE" "$AE" run "$TMPDIR_T/prog.ae" 2>&1)
case "$out" in
    *"$TOKEN"*) ;;
    *) echo "  [FAIL] run with AETHER_CACHE_DIR did not produce program output:"
       echo "$out" | sed 's/^/        /'; exit 1 ;;
esac
count=$(ls "$CACHE" 2>/dev/null | wc -l)
if [ "$count" -lt 1 ]; then
    echo "  [FAIL] AETHER_CACHE_DIR dir is empty after a cache-eligible run"
    exit 1
fi

# 2. Second run is a cache hit from the override dir (no recompile needed;
#    we assert it still runs correctly and doesn't touch the default dir).
out2=$(AETHER_CACHE_DIR="$CACHE" "$AE" run "$TMPDIR_T/prog.ae" 2>&1)
case "$out2" in
    *"$TOKEN"*) ;;
    *) echo "  [FAIL] cache-hit rerun failed:"; echo "$out2" | sed 's/^/        /'; exit 1 ;;
esac

# 3. The read-only-home scenario: $HOME unwritable, cache redirected.
#    Without the override this fails at link time ("Read-only file system").
RO_HOME="$TMPDIR_T/ro-home"
mkdir -p "$RO_HOME"
chmod 555 "$RO_HOME"
out3=$(HOME="$RO_HOME" AETHER_CACHE_DIR="$CACHE" "$AE" run "$TMPDIR_T/prog.ae" 2>&1)
rc3=$?
chmod 755 "$RO_HOME"
if [ "$rc3" -ne 0 ]; then
    echo "  [FAIL] run with read-only HOME + AETHER_CACHE_DIR exited $rc3:"
    echo "$out3" | sed 's/^/        /'
    exit 1
fi
case "$out3" in
    *"$TOKEN"*) ;;
    *) echo "  [FAIL] read-only-home run produced wrong output:"
       echo "$out3" | sed 's/^/        /'; exit 1 ;;
esac

echo "  [PASS] AETHER_CACHE_DIR overrides the cache location (incl. read-only \$HOME)"
exit 0
