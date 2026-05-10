#!/bin/sh
# Issue #334 regression: `make install` populates share/aether/contrib/
# with each contrib module's module.ae descriptor + headers, so an
# Aether program can `import contrib.<X>` and have the resolver find
# the descriptor without needing the upstream aether checkout living
# at a known relative path.
#
# Verifies:
#   1. install.sh writes contrib/<X>/module.ae for every module that
#      had one in the source tree.
#   2. install.sh trims the source-tree noise (.c, .m, tests/,
#      benchmarks/, example_*.ae, test_*.sh, build.sh, ci.sh) — the
#      install layout is descriptor-+-header only, no source.
#   3. The module.ae files are syntactically what the resolver looks
#      for: a non-empty file at the documented path.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

cd "$ROOT"

# Run install.sh against the temp prefix. Quiet — we only care about
# the resulting layout.
if ! ./install.sh "$TMPDIR" < /dev/null > "$TMPDIR/install.log" 2>&1; then
    echo "  [FAIL] install.sh exited non-zero"
    tail -20 "$TMPDIR/install.log"
    exit 1
fi

CONTRIB_INSTALL="$TMPDIR/share/aether/contrib"

if [ ! -d "$CONTRIB_INSTALL" ]; then
    echo "  [FAIL] $CONTRIB_INSTALL does not exist after install"
    exit 1
fi

# Every module.ae in the source contrib/ must have a counterpart in
# the install. Walk source-side and assert install-side presence.
missing=0
for src_module in $(find contrib -name 'module.ae' | sort); do
    rel="${src_module#contrib/}"
    target="$CONTRIB_INSTALL/$rel"
    if [ ! -f "$target" ]; then
        echo "  [FAIL] missing in install: $rel"
        missing=$((missing + 1))
    elif [ ! -s "$target" ]; then
        echo "  [FAIL] empty in install: $rel"
        missing=$((missing + 1))
    fi
done

if [ "$missing" -ne 0 ]; then
    echo "  [FAIL] $missing contrib module.ae file(s) missing or empty"
    exit 1
fi

# Source-tree noise must NOT have been copied. Hits would be
# regression of the trim step.
unwanted_count=$( {
    find "$CONTRIB_INSTALL" -type f -name '*.c'         2>/dev/null
    find "$CONTRIB_INSTALL" -type f -name '*.m'         2>/dev/null
    find "$CONTRIB_INSTALL" -type d -name tests         2>/dev/null
    find "$CONTRIB_INSTALL" -type d -name benchmarks    2>/dev/null
    find "$CONTRIB_INSTALL" -type f -name 'example_*.ae' 2>/dev/null
    find "$CONTRIB_INSTALL" -type f -name 'test_*.sh'   2>/dev/null
    find "$CONTRIB_INSTALL" -type f -name 'build.sh'    2>/dev/null
    find "$CONTRIB_INSTALL" -type f -name 'ci.sh'       2>/dev/null
} | wc -l | tr -d ' ')

if [ "$unwanted_count" -ne 0 ]; then
    echo "  [FAIL] install layout still contains source-tree noise:"
    {
        find "$CONTRIB_INSTALL" -type f -name '*.c'
        find "$CONTRIB_INSTALL" -type f -name '*.m'
        find "$CONTRIB_INSTALL" -type d -name tests
        find "$CONTRIB_INSTALL" -type d -name benchmarks
        find "$CONTRIB_INSTALL" -type f -name 'example_*.ae'
        find "$CONTRIB_INSTALL" -type f -name 'test_*.sh'
        find "$CONTRIB_INSTALL" -type f -name 'build.sh'
        find "$CONTRIB_INSTALL" -type f -name 'ci.sh'
    } | head -10
    exit 1
fi

# Spot-check a flagship module the issue called out by name.
for canary in sqlite tinyweb host/python; do
    if [ ! -f "$CONTRIB_INSTALL/$canary/module.ae" ]; then
        echo "  [FAIL] canary contrib module $canary not installed"
        exit 1
    fi
done

echo "  [PASS] contrib/ resolves system-wide after install (issue #334)"
