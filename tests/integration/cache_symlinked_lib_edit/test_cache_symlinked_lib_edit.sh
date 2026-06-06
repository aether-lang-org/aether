#!/bin/sh
# #623 regression: editing a module behind a SYMLINK on the
# `AETHER_LIB_DIR` search path must invalidate the build cache.
#
# Pre-fix bug: compute_cache_key only mtime'd the lib dir itself.
# A dir's mtime only bumps on create/delete/rename of an entry, NOT
# on an in-place edit of a file behind a symlink that points outside
# the dir. So `sed -i` of the symlink target left the dir mtime
# unchanged, the cache key stayed stable, `ae run` served the
# previously-compiled binary, and edits looked like no-ops until
# `rm -rf ~/.aether/cache`.
#
# Fix: compute_cache_key now walks each lib_dir's top-level entries
# and folds each entry's resolved-target (`stat`, follows symlinks)
# mtime + size into the key. ae also now seeds tc.lib_dirs[] from
# AETHER_LIB_DIR up front so the walk runs even when only the env
# var (no --lib flag) is set.

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] symlinks + bash heredoc — POSIX-only test"
        exit 0
        ;;
esac

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v ln >/dev/null 2>&1; then
    echo "  [SKIP] no ln(1)"; exit 0
fi

TMP="$(mktemp -d)"
CACHE_BACKUP=""
cleanup() {
    rm -rf "$TMP"
    if [ -n "$CACHE_BACKUP" ] && [ -d "$CACHE_BACKUP" ]; then
        rm -rf "$HOME/.aether/cache"
        mv "$CACHE_BACKUP" "$HOME/.aether/cache" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Move any existing cache aside so the test doesn't trample real builds.
if [ -d "$HOME/.aether/cache" ]; then
    CACHE_BACKUP="$HOME/.aether/cache.t623bk.$$"
    mv "$HOME/.aether/cache" "$CACHE_BACKUP"
fi
mkdir -p "$HOME/.aether/cache"

mkdir -p "$TMP/lib"
cat > "$TMP/mod_real.ae" <<'AE'
exports (greet)
greet() -> string { return "v1" }
AE
ln -s "$TMP/mod_real.ae" "$TMP/lib/mod.ae"

cat > "$TMP/main.ae" <<'AE'
import mod
main() { r = mod.greet()  println("got=${r}") }
AE

cd "$TMP"

# First run — establish baseline.
OUT1=$(AETHER_LIB_DIR="$TMP/lib" "$AE" run main.ae 2>&1 | tail -1)
if [ "$OUT1" != "got=v1" ]; then
    echo "  [FAIL] first run: expected 'got=v1', got '$OUT1'"
    exit 1
fi

# Bump mtime so a same-second edit isn't masked by 1-second mtime
# granularity on some filesystems. The fix folds in st_size too so
# size-changing edits invalidate even at the same second, but this
# test edits replace v1 → v2 (same size), so we need a real second
# of separation to be sure we're not relying on the size fallback.
sleep 1.1

# Edit the REAL file behind the symlink. Pre-fix bug: this didn't
# invalidate the cache because the symlink's mtime, the lib dir's
# mtime, and the cache key stayed unchanged.
sed -i 's/v1/v2/' "$TMP/mod_real.ae"

OUT2=$(AETHER_LIB_DIR="$TMP/lib" "$AE" run main.ae 2>&1 | tail -1)
if [ "$OUT2" != "got=v2" ]; then
    echo "  [FAIL] post-edit run: expected 'got=v2', got '$OUT2'"
    echo "  (pre-fix bug: edits behind a symlinked lib entry are missed)"
    exit 1
fi

# Bonus — also verify that an edit to a NON-symlinked module on the
# lib path invalidates (the original #413 promise; same code path
# now exercises real lstat-follows behaviour for both).
cat > "$TMP/lib/direct_mod.ae" <<'AE'
exports (val)
val() -> string { return "direct-v1" }
AE
cat > "$TMP/main2.ae" <<'AE'
import direct_mod
main() { r = direct_mod.val()  println("d=${r}") }
AE
OUT3=$(AETHER_LIB_DIR="$TMP/lib" "$AE" run main2.ae 2>&1 | tail -1)
[ "$OUT3" = "d=direct-v1" ] || {
    echo "  [FAIL] direct-mod baseline: expected 'd=direct-v1', got '$OUT3'"
    exit 1
}
sleep 1.1
sed -i 's/direct-v1/direct-v2/' "$TMP/lib/direct_mod.ae"
OUT4=$(AETHER_LIB_DIR="$TMP/lib" "$AE" run main2.ae 2>&1 | tail -1)
[ "$OUT4" = "d=direct-v2" ] || {
    echo "  [FAIL] direct-mod edit: expected 'd=direct-v2', got '$OUT4'"
    exit 1
}

echo "  [PASS] cache_symlinked_lib_edit: edits behind symlinked lib entries invalidate (#623)"
