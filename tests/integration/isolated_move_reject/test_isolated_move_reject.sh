#!/bin/sh
# Regression: #479 Isolated[T] move-only (linear) enforcement. The compiler
# must REJECT use-after-move of an Isolated value and the reuse of a heap
# source consumed by isolate(), and must ACCEPT the single-use / rebind /
# both-branch-consume patterns. Fixtures are inline so the generic .ae runner
# has nothing to pick up here (these are compile-outcome assertions, not
# run-to-exit-0 programs).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

fail() { echo "  [FAIL] isolated_move_reject: $1"; exit 1; }

# expect_reject <name> <source> : build must fail AND mention "use of moved".
expect_reject() {
    name="$1"; src="$2"
    printf '%s\n' "$src" > "$WORK/$name.ae"
    out="$("$AE" build "$WORK/$name.ae" -o "$WORK/$name" 2>&1)"
    if [ $? -eq 0 ]; then fail "$name: expected rejection, but it compiled"; fi
    printf '%s\n' "$out" | grep -q "use of moved value" \
        || fail "$name: rejected, but not with the move diagnostic. Got: $(printf '%s' "$out" | grep -i error | head -1)"
}

# expect_accept <name> <source> : build must succeed.
expect_accept() {
    name="$1"; src="$2"
    printf '%s\n' "$src" > "$WORK/$name.ae"
    if ! "$AE" build "$WORK/$name.ae" -o "$WORK/$name" >"$WORK/$name.log" 2>&1; then
        echo "  [FAIL] isolated_move_reject: $name: expected accept, but it failed:"
        grep -i error "$WORK/$name.log" | head -3
        exit 1
    fi
}

# --- rejections ---

expect_reject use_after_consume '
main() {
    let iso = isolate(7)
    let a = consume(iso)
    let b = consume(iso)
    println("${a} ${b}")
}'

expect_reject use_after_send '
message Task { n: int }
actor Worker { receive { Task -> { } } }
work(w: ActorRef[Worker]) {
    let iso = isolate(3)
    send(w, iso)
    send(w, iso)
}
main() { }'

expect_reject heap_source_reuse '
main() {
    let s = "hello"
    let iso = isolate(s)
    println("${s}")
    let v = consume(iso)
    println("${v}")
}'

expect_reject loop_external_consume '
main() {
    let iso = isolate(1)
    var i = 0
    while i < 3 {
        let v = consume(iso)
        println("${v}")
        i = i + 1
    }
}'

# --- acceptances (must NOT be over-rejected) ---

expect_accept single_use '
main() {
    let iso = isolate(9)
    let v = consume(iso)
    println("${v}")
}'

expect_accept both_branches_consume '
main() {
    let iso = isolate(5)
    let c = true
    if c {
        let a = consume(iso)
        println("${a}")
    } else {
        let b = consume(iso)
        println("${b}")
    }
}'

expect_accept fresh_each_iteration '
main() {
    var i = 0
    while i < 3 {
        let iso = isolate(i)
        let v = consume(iso)
        println("${v}")
        i = i + 1
    }
}'

expect_accept scalar_source_reusable '
main() {
    let i = 42
    let iso = isolate(i)
    let v = consume(iso)
    println("${i} ${v}")
}'

echo "  [PASS] isolated_move_reject: use-after-move rejected; single-use / both-branch / loop-fresh / scalar-source accepted"
exit 0
