#!/usr/bin/env bash
# contrib_build.sh — build static libs for each contrib module whose
# system dependency is present on this machine.
#
# Probes every contrib module (sqlite + the in-process host bridges).
# For each available dep, compiles the bridge to build/contrib/<x>.o
# and archives it as build/contrib/libaether_<x>.a. Skips modules
# whose dev libraries aren't installed — this is per-machine by
# design, matching the --with= capability-opt-in philosophy.
#
# Writes build/contrib/MANIFEST listing one line per built module:
#     <module>\t<archive_path>
# `make install-contrib` reads this manifest to know what to ship.
#
# Called from `make contrib`.

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/contrib"
mkdir -p "$OUT"

CC="${CC:-gcc}"
BASE_CFLAGS=(
    -O2 -fPIC -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
    -DAETHER_HAS_SANDBOX
    -I"$ROOT" -I"$ROOT/runtime" -I"$ROOT/runtime/actors"
    -I"$ROOT/runtime/scheduler" -I"$ROOT/runtime/utils"
    -I"$ROOT/runtime/memory" -I"$ROOT/runtime/config"
    -I"$ROOT/std" -I"$ROOT/std/string" -I"$ROOT/std/io"
    -I"$ROOT/std/math" -I"$ROOT/std/net" -I"$ROOT/std/collections"
    -I"$ROOT/std/json"
)

# probe_<lang> echoes dev-include flags on stdout when the dep is
# available. Returns 0 if available, 1 if not. (Mirror of the probe
# helpers in contrib_host_demos.sh — kept in sync, not sourced, so
# this script stays runnable even if the demos script is renamed.)
#
# Vendored-headers fallback: each probe checks /opt/aether/include/<lang>/
# FIRST (populated by tools/docker/aether-build's Containerfile heredoc
# from the pinned hosted-language-headers repo). Building the bridge .a
# only needs the headers; the actual runtime lib is dlopen'd by the
# bridge at end-user runtime, so the toolchain image stays slim — no
# distro -dev packages need to be installed.

# Standard vendored prefix in the aether-build image. Override for tests.
VENDORED_INC="${AETHER_CONTRIB_VENDORED_INC:-/opt/aether/include}"

probe_sqlite() {
    if pkg-config --exists sqlite3 2>/dev/null; then
        pkg-config --cflags-only-I sqlite3
        return 0
    fi
    # Fallback: header in default include path.
    if printf '#include <sqlite3.h>\nint main(){return 0;}\n' | \
        $CC -E -xc - >/dev/null 2>&1; then
        echo ""
        return 0
    fi
    return 1
}

probe_lua() {
    if [ -f "$VENDORED_INC/lua5.4/lua.h" ]; then
        echo "-I$VENDORED_INC/lua5.4"
        return 0
    fi
    for v in lua5.4 lua5.3 lua; do
        if pkg-config --exists "$v" 2>/dev/null; then
            pkg-config --cflags-only-I "$v"
            return 0
        fi
    done
    return 1
}

probe_python() {
    if [ -f "$VENDORED_INC/python/Python.h" ]; then
        echo "-I$VENDORED_INC/python"
        return 0
    fi
    if command -v python3-config >/dev/null 2>&1; then
        python3-config --includes
        return 0
    fi
    return 1
}

probe_ruby() {
    # Ruby has split portable + arch-specific headers in the vendored
    # layout (the hosted-language-headers repo mirrors what ruby-dev
    # ships — `ruby.h` includes `ruby/config.h` whose location is
    # arch-specific, hence the separate ruby-arch dir).
    if [ -f "$VENDORED_INC/ruby/ruby.h" ]; then
        echo "-I$VENDORED_INC/ruby -I$VENDORED_INC/ruby-arch"
        return 0
    fi
    for v in ruby-3.2 ruby-3.1 ruby-3.0 ruby; do
        if pkg-config --exists "$v" 2>/dev/null; then
            pkg-config --cflags-only-I "$v"
            return 0
        fi
    done
    return 1
}

probe_perl() {
    if [ -f "$VENDORED_INC/perl/perl.h" ]; then
        echo "-I$VENDORED_INC/perl"
        return 0
    fi
    if command -v perl >/dev/null 2>&1; then
        perl -MExtUtils::Embed -e ccopts 2>/dev/null && return 0
    fi
    return 1
}

probe_tcl() {
    # tcl is not in the current hosted-language-headers repo, so
    # vendored-fallback only kicks in if a future revision adds it.
    if [ -f "$VENDORED_INC/tcl/tcl.h" ]; then
        echo "-I$VENDORED_INC/tcl"
        return 0
    fi
    if pkg-config --exists tcl 2>/dev/null; then
        pkg-config --cflags-only-I tcl
        return 0
    fi
    sdk=$(xcrun --show-sdk-path 2>/dev/null) || true
    if [ -n "${sdk:-}" ] && [ -f "$sdk/usr/include/tcl.h" ]; then
        echo "-I$sdk/usr/include"
        return 0
    fi
    return 1
}

probe_js() {
    # The aether-build image vendors duktape.h flat in /opt/aether/include/.
    if [ -f "$VENDORED_INC/duktape.h" ]; then
        echo "-I$VENDORED_INC"
        return 0
    fi
    if pkg-config --exists duktape 2>/dev/null; then
        pkg-config --cflags-only-I duktape
        return 0
    fi
    return 1
}

# build_module <module_name> <relative_src_path> <AETHER_HAS_FLAG> <probe_fn>
build_module() {
    local name="$1"
    local src="$2"
    local flag="$3"
    local probe="$4"

    local incs
    if ! incs=$($probe 2>/dev/null); then
        printf "  %-18s SKIP (dev library not found)\n" "$name"
        return 1
    fi

    # shellcheck disable=SC2086
    if ! $CC "${BASE_CFLAGS[@]}" -D"$flag" $incs \
        -c "$ROOT/$src" -o "$OUT/$name.o" 2>"$OUT/$name.err"; then
        printf "  %-18s FAIL (compile)\n" "$name"
        sed 's/^/      /' "$OUT/$name.err" | head -5
        return 1
    fi
    rm -f "$OUT/$name.err"

    if ! ar rcs "$OUT/libaether_$name.a" "$OUT/$name.o"; then
        printf "  %-18s FAIL (archive)\n" "$name"
        return 1
    fi

    printf "  %-18s OK   build/contrib/libaether_%s.a\n" "$name" "$name"
    echo -e "$name\tbuild/contrib/libaether_$name.a" >> "$OUT/MANIFEST.tmp"
    return 0
}

echo "==================================="
echo "  Building contrib modules"
echo "==================================="
echo ""

: > "$OUT/MANIFEST.tmp"

built=0
skipped=0

# sqlite
if build_module sqlite        contrib/sqlite/aether_sqlite.c           AETHER_HAS_SQLITE  probe_sqlite; then
    built=$((built + 1)); else skipped=$((skipped + 1)); fi

# In-process host bridges. Each bridge file gates its real body behind
# AETHER_HAS_<LANG>; without the flag it compiles to a stub. We always
# compile with the flag (since the probe just succeeded) — that's the
# whole point of building a per-machine artifact.
if build_module host_python   contrib/host/python/aether_host_python.c AETHER_HAS_PYTHON  probe_python; then
    built=$((built + 1)); else skipped=$((skipped + 1)); fi
if build_module host_lua      contrib/host/lua/aether_host_lua.c       AETHER_HAS_LUA     probe_lua; then
    built=$((built + 1)); else skipped=$((skipped + 1)); fi
if build_module host_perl     contrib/host/perl/aether_host_perl.c     AETHER_HAS_PERL    probe_perl; then
    built=$((built + 1)); else skipped=$((skipped + 1)); fi
if build_module host_ruby     contrib/host/ruby/aether_host_ruby.c     AETHER_HAS_RUBY    probe_ruby; then
    built=$((built + 1)); else skipped=$((skipped + 1)); fi
if build_module host_js       contrib/host/js/aether_host_js.c         AETHER_HAS_JS      probe_js; then
    built=$((built + 1)); else skipped=$((skipped + 1)); fi
if build_module host_tcl      contrib/host/tcl/aether_host_tcl.c       AETHER_HAS_TCL     probe_tcl; then
    built=$((built + 1)); else skipped=$((skipped + 1)); fi

mv "$OUT/MANIFEST.tmp" "$OUT/MANIFEST"

echo ""
echo "  $built built, $skipped skipped"
echo ""
echo "  Manifest: $OUT/MANIFEST"
echo "  Install:  make install-contrib"
