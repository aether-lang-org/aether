#!/usr/bin/env bash
# contrib_build.sh — build static libs for contrib modules.
#
# Two modes:
#
# 1. Default (MODULES unset) — best-effort: probe every contrib module
#    (sqlite + host bridges). For each available dep, compile + archive.
#    Skip + tally any whose dev libs aren't installed. Same shape as
#    historical behaviour; convenient for dev boxes.
#
# 2. MODULES=<list> (comma-separated) — explicit, fail-hard: build ONLY
#    the named modules; any requested module that can't compile is a
#    hard failure (exit 1), not a silent skip. This is the mode the
#    `aether-build --with=<lang>` path uses, where the caller has already
#    `apt install`ed the matching -dev kit and expects the bridge to
#    build. Silent SKIP in that path is the v0.209.0 footgun
#    (ctr_notes.md Bug 6) we're closing here.
#
# Examples:
#   MODULES=python                    # build only host_python; fail if it can't
#   MODULES=python,lua                # build python AND lua; fail if either can't
#   (unset)                           # build-all, tolerate skips
#
# Writes build/contrib/MANIFEST listing one line per built module:
#     <module>\t<archive_path>
# `make install-contrib` reads this manifest to know what to ship.
#
# Called from `make contrib` (default mode) or
# `make contrib MODULES=<list>` (explicit mode).

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
# Each probe queries the distro's REAL -dev kit via pkg-config or a
# language-specific config tool (python3-config / perl -MExtUtils::Embed).
# The earlier vendored-fallback path
# (`/opt/aether/include/<lang>/` populated from
# hosted-language-headers) was removed when ctr_notes.md Bug 6 proved
# pyconfig.h / luaconf.h / perl.h are distro-generated multiarch
# dispatchers that can't be portably vendored. The aether-build image
# now apt-installs the matching -dev kit per `--with=<lang>` layer.

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
    for v in lua5.4 lua5.3 lua; do
        if pkg-config --exists "$v" 2>/dev/null; then
            pkg-config --cflags-only-I "$v"
            return 0
        fi
    done
    return 1
}

probe_python() {
    if command -v python3-config >/dev/null 2>&1; then
        python3-config --includes
        return 0
    fi
    return 1
}

probe_ruby() {
    for v in ruby-3.2 ruby-3.1 ruby-3.0 ruby; do
        if pkg-config --exists "$v" 2>/dev/null; then
            pkg-config --cflags-only-I "$v"
            return 0
        fi
    done
    return 1
}

probe_perl() {
    if command -v perl >/dev/null 2>&1; then
        perl -MExtUtils::Embed -e ccopts 2>/dev/null && return 0
    fi
    return 1
}

probe_tcl() {
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

probe_duktape() {
    if pkg-config --exists duktape 2>/dev/null; then
        pkg-config --cflags-only-I duktape
        return 0
    fi
    # Fallback: header in default include path (Debian's duktape-dev
    # installs duktape.h at /usr/include/duktape.h without a .pc file).
    if printf '#include <duktape.h>\nint main(){return 0;}\n' | \
        $CC -E -xc - >/dev/null 2>&1; then
        echo ""
        return 0
    fi
    return 1
}

# build_module <module_name> <relative_src_path> <AETHER_HAS_FLAG> <probe_fn>
# Returns: 0 OK, 1 SKIP (probe failed), 2 FAIL (compile/archive failed)
# Caller decides whether SKIP or FAIL is fatal (depends on MODULES mode).
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
        sed 's/^/      /' "$OUT/$name.err" | head -10
        return 2
    fi
    rm -f "$OUT/$name.err"

    if ! ar rcs "$OUT/libaether_$name.a" "$OUT/$name.o"; then
        printf "  %-18s FAIL (archive)\n" "$name"
        return 2
    fi

    printf "  %-18s OK   build/contrib/libaether_%s.a\n" "$name" "$name"
    echo -e "$name\tbuild/contrib/libaether_$name.a" >> "$OUT/MANIFEST.tmp"
    return 0
}

# Catalogue: maps a friendly --with name (or `make contrib MODULES=` entry)
# to the build_module argv for that module. Add new bridges here.
#
# Format (one per line): <short-name>|<build_module args...>
# `short-name` is what users pass in MODULES= / --with=. The build_module
# args are exactly what the build loop calls.
CATALOGUE=(
    "sqlite|sqlite       contrib/sqlite/aether_sqlite.c           AETHER_HAS_SQLITE  probe_sqlite"
    "python|host_python  contrib/host/python/aether_host_python.c AETHER_HAS_PYTHON  probe_python"
    "lua|host_lua        contrib/host/lua/aether_host_lua.c       AETHER_HAS_LUA     probe_lua"
    "perl|host_perl      contrib/host/perl/aether_host_perl.c     AETHER_HAS_PERL    probe_perl"
    "ruby|host_ruby      contrib/host/ruby/aether_host_ruby.c     AETHER_HAS_RUBY    probe_ruby"
    "duktape|host_duktape  contrib/host/duktape/aether_host_duktape.c  AETHER_HAS_DUKTAPE  probe_duktape"
    "tcl|host_tcl        contrib/host/tcl/aether_host_tcl.c       AETHER_HAS_TCL     probe_tcl"
)

# Find a catalogue line by short name. Echoes the build_module arg list
# on stdout, returns 1 if the name isn't in the catalogue.
catalogue_lookup() {
    local want="$1"
    local entry
    for entry in "${CATALOGUE[@]}"; do
        local short="${entry%%|*}"
        if [ "$short" = "$want" ]; then
            echo "${entry#*|}"
            return 0
        fi
    done
    return 1
}

# Decide what we're building. Two modes:
MODULES="${MODULES:-}"
if [ -n "$MODULES" ]; then
    MODE="explicit"
    # Comma -> space, dedupe, sort. The aether-build --with= path expects
    # build order to be deterministic so layer caching stays effective.
    REQUESTED=$(echo "$MODULES" | tr ',' '\n' | sort -u | xargs)
else
    MODE="default"
    # Default: build everything in the catalogue, skip silently if a dep
    # is missing. Same as the historical behaviour.
    REQUESTED=$(printf '%s\n' "${CATALOGUE[@]}" | cut -d'|' -f1 | xargs)
fi

echo "==================================="
echo "  Building contrib modules ($MODE mode)"
echo "==================================="
echo "  Requested: $REQUESTED"
echo ""

: > "$OUT/MANIFEST.tmp"

built=0
skipped=0
failed=0
hard_failures=""

for short in $REQUESTED; do
    args=$(catalogue_lookup "$short") || {
        echo "  $short — unknown module name (not in CATALOGUE)" >&2
        if [ "$MODE" = "explicit" ]; then
            hard_failures="$hard_failures $short(unknown)"
            failed=$((failed + 1))
        else
            skipped=$((skipped + 1))
        fi
        continue
    }
    # shellcheck disable=SC2086
    build_module $args
    rc=$?
    case "$rc" in
        0) built=$((built + 1)) ;;
        1)
            # SKIP — probe said the dep isn't here. In default mode this
            # is fine. In explicit mode the caller asked for this module,
            # so a missing dep is a hard failure.
            if [ "$MODE" = "explicit" ]; then
                hard_failures="$hard_failures $short(missing dep)"
                failed=$((failed + 1))
            else
                skipped=$((skipped + 1))
            fi
            ;;
        2)
            # FAIL — compile or archive failed. Always a hard failure
            # (no env-dependent toggle: the source itself is broken,
            # not the host's package state).
            hard_failures="$hard_failures $short(compile/archive)"
            failed=$((failed + 1))
            ;;
    esac
done

mv "$OUT/MANIFEST.tmp" "$OUT/MANIFEST"

echo ""
if [ "$MODE" = "explicit" ] && [ "$failed" -gt 0 ]; then
    echo "  $built built, $failed FAILED, $skipped skipped"
    echo ""
    echo "  Failures (explicit mode — these were requested but couldn't build):"
    echo "   $hard_failures"
    echo ""
    echo "  Manifest: $OUT/MANIFEST"
    exit 1
fi

echo "  $built built, $skipped skipped"
echo ""
echo "  Manifest: $OUT/MANIFEST"
echo "  Install:  make install-contrib"
