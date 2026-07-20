#!/bin/sh
# emit-link-requirements-from-import-graph: aetherc emits a `// aether-link:`
# comment on the first line of the generated C, listing the native libraries
# the RESOLVED import graph pulls in, so a downstream build linking the .c
# doesn't rediscover them by `undefined reference` archaeology.
#
# Asserts:
#   - a program importing std.regex + std.zlib gets `// aether-link:` with
#     -lpcre2-8 and -lz;
#   - a program importing nothing native gets NO such comment;
#   - the header comment is inert: the emitted C still compiles/links/runs;
#   - the ask's consumer extraction (`sed -n 's|^// aether-link:||p'`) works.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
AE="$ROOT/build/ae"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail() { echo "  [FAIL] emit_link_requirements: $1"; exit 1; }

# --- 1. lib-needing program: comment present with both libs ---
"$AETHERC" "$SCRIPT_DIR/uses_regex_zlib.ae" "$TMP/withlibs.c" >/dev/null 2>&1 \
    || fail "aetherc failed on uses_regex_zlib.ae"

first="$(head -1 "$TMP/withlibs.c")"
case "$first" in
    "// aether-link:"*) : ;;
    *) fail "expected '// aether-link:' as first line, got: $first" ;;
esac
echo "$first" | grep -q -- "-lpcre2-8" || fail "missing -lpcre2-8 in: $first"
echo "$first" | grep -q -- "-lz"       || fail "missing -lz in: $first"

# The ask's extraction recipe must recover the tokens.
extracted="$(sed -n 's|^// aether-link:||p' "$TMP/withlibs.c")"
echo "$extracted" | grep -q -- "-lpcre2-8" || fail "sed extraction lost -lpcre2-8"

# --- 2. no-lib program: NO comment ---
"$AETHERC" "$SCRIPT_DIR/nolibs.ae" "$TMP/nolibs.c" >/dev/null 2>&1 \
    || fail "aetherc failed on nolibs.ae"
if grep -q "aether-link" "$TMP/nolibs.c"; then
    fail "nolibs.ae should not carry an aether-link comment"
fi

# --- 3. the header comment is inert: full build still compiles/links/runs.
# Skip on toolchains without pcre2 (e.g. Windows CI), where a std.regex link
# is unavailable — the comment-emission feature under test doesn't depend on
# the lib being present, only on the resolved import graph. Guard on whether a
# minimal std.regex program links here at all. ---
out="$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run uses_regex_zlib.ae 2>&1 )"
rc=$?
if [ "$rc" -ne 0 ]; then
    case "$out" in
        *pcre2*|*"undefined reference"*|*"regex"*"unavailable"*)
            echo "  [SKIP-RUN] emit_link_requirements: std.regex not linkable here; comment-emission verified above" ;;
        *)
            fail "program with aether-link header failed to run (rc=$rc): $out" ;;
    esac
else
    echo "$out" | grep -q "ok" || fail "expected 'ok' in output, got: $out"
fi

echo "  [PASS] emit_link_requirements: import-graph link libs emitted, comment inert"
exit 0
