#!/bin/sh
# Regression test for issue #436 facet B — flat C-symbol namespace
# causing collisions with libc / POSIX symbols.
#
# The original failure mode: Aether code defining a function called
# `bind` (or `connect`, `listen`, `read`, `write`, etc.) would emit a
# bare C function of the same name. At link time the linker would
# silently pick libc's symbol — Aether's `bind` either never got
# called or got picked when libc's was expected, producing impossible-
# to-debug runtime behaviour.
#
# Fix (this PR): codegen's `is_c_reserved_word` list expanded to
# cover the libc network sockets API + POSIX I/O + process control +
# memory + string + dynamic linking + time/env symbol space. Hits
# get auto-prefixed with `ae_` by `safe_c_name`, identically to the
# pre-existing handling of `read`, `write`, `malloc`, etc.
#
# Each case below:
#   1. defines an Aether function with a libc-colliding name
#   2. exercises that function from main()
#   3. asserts the program runs AND prints the expected value
#      (proving Aether's symbol got called, not libc's)
#   4. additionally for one case, asserts the emitted C symbol is
#      `ae_<name>` (the documented prefix convention) via `nm`.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

pass=0; fail=0

# Test a single libc-colliding name. Each case writes a tiny Aether
# program, builds it, runs it, asserts the printed value matches.
run_collision_case() {
    fn_name="$1"
    expected="$2"

    cat > "$TMPDIR/${fn_name}_test.ae" <<EOF
${fn_name}() -> int {
    return ${expected}
}

main() {
    n = ${fn_name}()
    println("\${n}")
}
EOF
    out=$("$AE" run "$TMPDIR/${fn_name}_test.ae" 2>&1 | tail -1)
    if [ "$out" = "$expected" ]; then
        echo "  [PASS] user function named '${fn_name}' runs correctly (printed ${out})"
        pass=$((pass + 1))
    else
        echo "  [FAIL] user function named '${fn_name}' did not return expected value"
        echo "    expected: $expected"
        echo "    got:      $out"
        fail=$((fail + 1))
    fi
}

# Cases covering each category of the expanded reserved list.
# Note: `send` is also an Aether *keyword* (actor messaging), so the
# parser rejects it as a function name with E0100 — that's a layer
# of defence above the codegen mitigation here. Same goes for any
# future libc symbol that overlaps with an Aether keyword. We test
# only the libc-only names below; the keyword-collision case is
# already exercised by the existing parser tests.
run_collision_case "bind"     42   # network sockets API
run_collision_case "listen"   1    # network sockets API
run_collision_case "accept"   7    # network sockets API
run_collision_case "connect"  100  # network sockets API
run_collision_case "recv"     3    # network sockets API
run_collision_case "select"   99   # network sockets API
run_collision_case "pipe"     8    # POSIX I/O
run_collision_case "fork"     0    # process control
run_collision_case "kill"     42   # process control
run_collision_case "stat"     17   # POSIX I/O
run_collision_case "chmod"    644  # POSIX I/O

# Additional integration: a function `bind` AND a function `listen`
# in the same program both work (no cross-contamination).
cat > "$TMPDIR/multi.ae" <<EOF
bind() -> int { return 11 }
listen() -> int { return 22 }
accept() -> int { return 33 }

main() {
    a = bind()
    b = listen()
    c = accept()
    println("\${a},\${b},\${c}")
}
EOF
out=$("$AE" run "$TMPDIR/multi.ae" 2>&1 | tail -1)
if [ "$out" = "11,22,33" ]; then
    echo "  [PASS] multiple libc-colliding names coexist in one program"
    pass=$((pass + 1))
else
    echo "  [FAIL] multi-collision case: expected '11,22,33', got '$out'"
    fail=$((fail + 1))
fi

# Symbol-level verification: emit C for the `bind` case, grep for
# the prefixed symbol. Only run when `aetherc` supports --emit-c
# (always true today; defensive in case the flag moves).
if "$AETHERC" --help 2>&1 | grep -q -- "--emit-c"; then
    emitted=$("$AETHERC" --emit-c "$TMPDIR/bind_test.ae" 2>&1)
    if printf '%s\n' "$emitted" | grep -qE "(^|[^a-zA-Z_])ae_bind\("; then
        echo "  [PASS] emitted C symbol is 'ae_bind' (libc-collision-free)"
        pass=$((pass + 1))
    else
        echo "  [FAIL] emitted C did not contain the documented 'ae_bind' prefix"
        printf '%s\n' "$emitted" | grep -E "bind" | sed 's/^/      /'
        fail=$((fail + 1))
    fi
    # Negative: the bare `bind(` symbol must NOT be defined as a
    # function (a forward decl to libc bind via a system header is
    # allowed; what we're guarding against is `int bind() { … }`).
    if printf '%s\n' "$emitted" | grep -qE "^int bind\(\)[ ]*\{"; then
        echo "  [FAIL] emitted C still defines a bare 'bind' function (libc collision)"
        fail=$((fail + 1))
    else
        echo "  [PASS] emitted C does NOT define a bare 'bind' function"
        pass=$((pass + 1))
    fi
fi

echo
echo "c_symbol_namespace: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
