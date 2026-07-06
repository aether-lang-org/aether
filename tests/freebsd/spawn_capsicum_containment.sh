#!/bin/sh
# tests/freebsd/spawn_capsicum_containment.sh — end-to-end proof that
# spawn_sandboxed auto-wires Capsicum on FreeBSD (issue #1003, gap 2).
#
# Contract under test: spawn_sandboxed sets AETHER_CAPSICUM=1 in every
# child's environment (runtime/sandbox/spawn_sandboxed_bsd.c), and an
# Aether child self-enters capability mode at startup via the hook in
# runtime/sandbox/capsicum_autosandbox.c — so a spawned Aether child is
# kernel-contained WITHOUT its source ever calling capsicum.enter().
#
# Shape mirrors tests/integration/sandbox_toolchain: build the preload
# .so and both binaries into a tmpdir, run the parent from there (the
# preload is discovered next to the running binary), and assert on the
# child's own report of its containment state.
#
# Exit: 0 = pass, 1 = fail, 2 = skip (not FreeBSD). Run by run.sh.
set -u

[ "$(uname -s 2>/dev/null)" = "FreeBSD" ] || { echo "SKIP: not FreeBSD"; exit 2; }

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
AE="${AE:-$ROOT/build/ae}"

[ -x "$AE" ] || { echo "FAIL: ae not found at $AE"; exit 1; }

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# Preload library: prefer the one the build produced; else compile it
# the way the Makefile does on FreeBSD (dlopen/shm_open live in libc,
# no -ldl -lrt).
if [ -f "$ROOT/build/libaether_sandbox.so" ]; then
    cp "$ROOT/build/libaether_sandbox.so" "$TMPDIR/"
else
    cc -shared -fPIC -o "$TMPDIR/libaether_sandbox.so" \
        "$ROOT/runtime/libaether_sandbox_preload.c" 2>"$TMPDIR/preload-cc.log" || {
        echo "FAIL: preload .so build failed:"; cat "$TMPDIR/preload-cc.log"; exit 1
    }
fi

# The child NEVER calls capsicum.enter() itself — if it lands in
# capability mode, spawn_sandboxed's auto-wiring put it there.
cat > "$TMPDIR/child.ae" <<'AE'
import std.capsicum
import std.os
extern println(s: string)
main() {
    m = capsicum.in_mode()
    if m == 1 {
        println("child-in-capability-mode")
        exit(0)
    }
    println("child-NOT-contained mode=${m}")
    exit(1)
}
AE

cat > "$TMPDIR/parent.ae" <<AE
import std.list
extern println(s: string)
main() {
    g = list.new()
    list.add(g, "env");     list.add(g, "*")
    list.add(g, "fs_read"); list.add(g, "/*")
    list.add(g, "native");  list.add(g, "*")
    rc = spawn_sandboxed(g, "$TMPDIR/child", "unused")
    println("rc=\${rc}")
    list.free(g)
}
AE

"$AE" build "$TMPDIR/child.ae" -o "$TMPDIR/child" >"$TMPDIR/child-build.log" 2>&1 || {
    echo "FAIL: child build failed:"; tail -20 "$TMPDIR/child-build.log"; exit 1
}
"$AE" build "$TMPDIR/parent.ae" -o "$TMPDIR/parent" >"$TMPDIR/parent-build.log" 2>&1 || {
    echo "FAIL: parent build failed:"; tail -20 "$TMPDIR/parent-build.log"; exit 1
}

cd "$TMPDIR" || exit 1
./parent > out.log 2>&1
cat out.log

grep -q '^child-in-capability-mode$' out.log || {
    echo "FAIL: spawned Aether child was not in capability mode"; exit 1
}
grep -q '^rc=0$' out.log || {
    echo "FAIL: spawn_sandboxed did not return the child's success exit"; exit 1
}
echo "PASS: spawn_sandboxed auto-contains Aether children via Capsicum"
exit 0
