#!/bin/sh
# Regression: fs.join_clean lexically cleans path_join's result so a
# caller-supplied `..`/`.` segment can't leave a traversal in the path
# that hits the filesystem (path-traversal-defense invariant), and
# fs.first_element returns the first cleaned path component. Both were
# added so downstream object-store code (fbs-core) can drop its
# pathutil.join wrapper.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] fs_join_clean: $AE not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
cd "$tmpdir" || exit 1

cat > join_probe.ae <<'EOF'
import std.fs

check(label: string, got: string, want: string) {
    if got != want {
        println("FAIL: ${label}: got '${got}' want '${want}'")
        return
    }
    println("ok: ${label}")
}

main() {
    // join_clean collapses a traversal in the tail segment
    check("traversal", fs.join_clean("bucket", "a/../b"), "bucket/b")
    check("dotslash", fs.join_clean("/data", "x/./y"), "/data/x/y")
    check("empty-a", fs.join_clean("", "a/../b"), "b")
    check("empty-b", fs.join_clean("bucket", ""), "bucket")
    check("plain", fs.join_clean("p", "q"), "p/q")

    // first_element: leading cleaned component
    check("fe-multi", fs.first_element("a/b/c"), "a")
    check("fe-rooted", fs.first_element("/a/b"), "a")
    check("fe-traversal", fs.first_element("x/../y/z"), "y")
    check("fe-single", fs.first_element("only"), "only")
}
EOF

if ! "$AE" build join_probe.ae -o jc >"$tmpdir/build.log" 2>&1; then
    echo "  [FAIL] fs_join_clean: build failed"
    cat "$tmpdir/build.log"
    exit 1
fi

if [ -x ./jc ]; then
    out="$(./jc 2>&1)"
elif [ -x ./jc.exe ]; then
    out="$(./jc.exe 2>&1)"
else
    echo "  [FAIL] fs_join_clean: built binary not found"
    ls -la
    exit 1
fi

if echo "$out" | grep -q "FAIL"; then
    echo "  [FAIL] fs_join_clean:"
    echo "$out"
    exit 1
fi

# Expect 9 ok lines
n="$(echo "$out" | grep -c '^ok:')"
if [ "$n" -ne 9 ]; then
    echo "  [FAIL] fs_join_clean: expected 9 ok lines, got $n"
    echo "$out"
    exit 1
fi

echo "  [PASS] fs_join_clean: join_clean cleans traversals; first_element returns leading segment"
exit 0
