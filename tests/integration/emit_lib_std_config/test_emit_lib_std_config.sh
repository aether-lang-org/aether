#!/bin/sh
# Regression: `--emit=lib` on a module that imports std.config must link.
#
# std/config exported C symbols named aether_config_get/_has/_put/...,
# colliding with the embed ABI of the same name in runtime/aether_config.c
# (different signatures: the embed ABI takes an AetherValue* root, the
# store takes a bare key). `ae build --emit=lib` appends
# runtime/aether_config.c to the link AND links libaether.a, so any
# library using std.config died with:
#
#     duplicate symbol '_aether_config_has'
#
# The store's C symbols are now aether_config_store_*, leaving the
# documented embed ABI untouched.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] emit_lib_std_config on Windows (POSIX shared-lib build)"
        exit 0
        ;;
esac

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

cat > "$TMPDIR/cfglib.ae" <<'EOF'
import std.config

exports (setup, lookup)

setup() {
    config.put("env", "prod")
}

lookup(key: string) -> string {
    return config.get_or(key, "unset")
}
EOF

if ! "$ROOT/build/ae" build --emit=lib "$TMPDIR/cfglib.ae" -o "$TMPDIR/libcfg" \
        >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] --emit=lib with std.config failed to link"
    grep -iE "duplicate symbol|error" "$TMPDIR/build.log" | head -5
    exit 1
fi

if grep -q "duplicate symbol" "$TMPDIR/build.log"; then
    echo "  [FAIL] duplicate symbol reported during link"
    grep "duplicate symbol" "$TMPDIR/build.log" | head -3
    exit 1
fi

FOUND=""
for candidate in "$TMPDIR/libcfg" "$TMPDIR/libcfg.so" "$TMPDIR/libcfg.dylib"; do
    [ -f "$candidate" ] && { FOUND="$candidate"; break; }
done

if [ -z "$FOUND" ]; then
    echo "  [FAIL] link reported success but no shared library was produced"
    exit 1
fi

echo "  [PASS] emit_lib_std_config: std.config links cleanly under --emit=lib"
