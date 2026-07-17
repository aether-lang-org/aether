#!/usr/bin/env bash
#
# Cross-compilation smoke test for `ae build --target=<triple>` (#1105).
#
# Verifies the zig cc backend: each supported target produces a binary of
# the right object format, and the two error paths (unknown target, a
# program importing an external-lib module) report cleanly. Cross builds
# for a foreign OS are not runnable on the build host, so this checks the
# emitted object format via file(1) rather than executing them; the two
# host-runnable targets are additionally executed.
#
# Requires zig on PATH. When zig is absent (e.g. CI without the zig
# toolchain) the whole script SKIPS with exit 0 so it never breaks a gate.
#
# Run with: make test-cross   (or: bash tests/scripts/cross_compile.sh)

set -u
cd "$(dirname "$0")/../.." || exit 1
ROOT="$(pwd)"
AE="$ROOT/build/ae"

pass=0; fail=0
ok()   { printf '  \033[0;32mOK\033[0m    %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  \033[0;31mFAIL\033[0m  %s\n' "$1"; fail=$((fail+1)); }

if ! command -v zig >/dev/null 2>&1; then
    echo "SKIP: zig not installed; cross-compilation test requires zig on PATH."
    exit 0
fi
if [ ! -x "$AE" ]; then
    echo "SKIP: build/ae not found; run 'make ae' first."
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
HELLO="examples/basics/hello.ae"
ACTOR="examples/actors/counter.ae"
HTTP="examples/actors/site-poller.ae"

echo "Cross-compilation targets ($(zig version)):"

# target triple | file(1) substring that must appear | runnable-here flag
check_target() {
    local triple="$1" want="$2" run="$3" src="$4"
    local out="$TMP/out_${triple}"
    if ! "$AE" build --target="$triple" "$src" -o "$out" >/dev/null 2>"$TMP/err"; then
        bad "$triple: build failed"; sed 's/^/        /' "$TMP/err"; return
    fi
    local desc; desc="$(file -b "$out")"
    if ! printf '%s' "$desc" | grep -q "$want"; then
        bad "$triple: expected '$want', got '$desc'"; return
    fi
    if [ "$run" = "run" ]; then
        if ! "$out" >/dev/null 2>&1; then bad "$triple: built but did not run"; return; fi
        ok "$triple (built + ran): $desc"
    else
        ok "$triple: $desc"
    fi
}

# arch/OS format checks. macos targets run on this host if it is macOS.
host_os="$(uname -s)"
mac_run="nofile"; [ "$host_os" = "Darwin" ] && mac_run="run"
check_target "x86_64-linux"   "ELF 64-bit"          "nofile"   "$HELLO"
check_target "aarch64-linux"  "ELF 64-bit"          "nofile"   "$HELLO"
check_target "aarch64-macos"  "Mach-O 64-bit"       "$mac_run" "$ACTOR"
check_target "x86_64-macos"   "Mach-O 64-bit"       "$mac_run" "$ACTOR"

# Regression: a program defining a top-level function named like an
# unreferenced runtime global (e.g. `describe` in aether_host.c) must
# cross-build, exactly as it builds natively. Guards the archive-based
# link that gives on-demand object pulling. sum-types.ae defines `describe`.
if [ -f "examples/basics/sum-types.ae" ]; then
    if "$AE" build --target=x86_64-linux examples/basics/sum-types.ae -o "$TMP/st" \
         >/dev/null 2>"$TMP/err"; then
        ok "symbol-collision program (defines 'describe') cross-builds"
    else
        bad "symbol-collision program failed to cross-build"; sed 's/^/        /' "$TMP/err"
    fi
fi

# Error path: --emit=lib with a cross target is rejected up front.
if "$AE" build --target=x86_64-linux --emit=lib "$HELLO" -o "$TMP/xl" >/dev/null 2>"$TMP/err"; then
    bad "--emit=lib cross: build unexpectedly succeeded"
elif grep -q "executables only" "$TMP/err"; then
    ok "--emit=lib cross rejected"
else
    bad "--emit=lib cross: wrong error"; sed 's/^/        /' "$TMP/err"
fi

# Error path: unknown target (no zig needed, but exercised here anyway).
if "$AE" build --target=sparc-solaris "$HELLO" -o "$TMP/x" >/dev/null 2>"$TMP/err"; then
    bad "unknown target: build unexpectedly succeeded"
elif grep -q "Unknown target" "$TMP/err"; then
    ok "unknown target rejected"
else
    bad "unknown target: wrong error"; sed 's/^/        /' "$TMP/err"
fi

# A program using a library-backed module (std.http) still cross-builds,
# with a note that those features report unavailable at runtime (matching a
# native build without OpenSSL/zlib/nghttp2/PCRE2).
if [ -f "$HTTP" ]; then
    if "$AE" build --target=x86_64-linux "$HTTP" -o "$TMP/xhttp" >/dev/null 2>"$TMP/err" \
         && [ -f "$TMP/xhttp" ]; then
        if grep -q "built without OpenSSL" "$TMP/err"; then
            ok "library-backed program cross-builds with an availability note"
        else
            bad "library-backed program built but emitted no note"; sed 's/^/        /' "$TMP/err"
        fi
    else
        bad "library-backed program failed to cross-build"; sed 's/^/        /' "$TMP/err"
    fi
fi

echo ""
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
