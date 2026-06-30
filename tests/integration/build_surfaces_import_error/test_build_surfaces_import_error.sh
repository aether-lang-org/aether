#!/bin/sh
# Regression (#953): `ae build` must fail — non-zero exit, no binary — when an
# IMPORTED module has a compile error that `ae check` reports. Before the fix,
# build parsed the imported module, recovered from its parse error (the
# parser's error recovery dropped the offending `@ 1`), type-checked the
# resulting valid-looking AST, and produced a WORKING binary from source that
# does not compile — silently disagreeing with `check`. `build` and `check`
# must agree on validity.
#
# A control case (a valid import) guards against the gate being over-broad:
# it keys on the global error count, so a clean import must still build.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] build_surfaces_import_error: ae not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
cd "$SCRIPT_DIR" || exit 1

fail=0

# --- Case 1: import does NOT compile -> check and build must BOTH reject ---
"$AE" check app.ae >"$tmpdir/check.log" 2>&1
check_rc=$?
"$AE" build app.ae -o "$tmpdir/bin" >"$tmpdir/build.log" 2>&1
build_rc=$?

if [ "$check_rc" -eq 0 ]; then
    echo "  [FAIL] build_surfaces_import_error: ae check accepted a program whose import does not compile"
    fail=1
fi
if [ "$build_rc" -eq 0 ]; then
    echo "  [FAIL] build_surfaces_import_error: ae build accepted a program whose import does not compile (rc=0)"
    fail=1
fi
if [ -f "$tmpdir/bin" ]; then
    echo "  [FAIL] build_surfaces_import_error: ae build produced a binary from non-compiling source"
    fail=1
fi
if ! grep -q 'error' "$tmpdir/build.log"; then
    echo "  [FAIL] build_surfaces_import_error: ae build did not surface the imported module's error"
    sed 's/^/        /' "$tmpdir/build.log" | head -8
    fail=1
fi

# --- Case 2 (control): a VALID import must still build and run ---
"$AE" build appok.ae -o "$tmpdir/okbin" >"$tmpdir/ok.log" 2>&1
ok_rc=$?
if [ "$ok_rc" -ne 0 ] || [ ! -f "$tmpdir/okbin" ]; then
    echo "  [FAIL] build_surfaces_import_error: a valid imported module no longer builds (false positive)"
    sed 's/^/        /' "$tmpdir/ok.log" | head -8
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "  [PASS] build_surfaces_import_error: build and check agree; bad import fails with no binary, clean import builds"
fi
exit $fail
