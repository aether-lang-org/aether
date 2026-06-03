#!/bin/sh
# Regression: ${VAR} expansion in aether.toml [build] cflags / link_flags.
#
# Honoured for the AETHER_*-prefixed allowlist only; unset names warn
# and expand to empty; non-allowlisted names warn and expand to empty.
#
# Each subtest runs from its own tmpdir so the ae build-cache (which is
# keyed by source hash, not by environment) doesn't return a stale
# success from a previous test cell.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

fail=0
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# Generate a fresh project dir under $tmpdir with the given toml + a
# probe whose marker name is unique per test cell (defeats the build
# cache so each cell exercises a real compile).
make_proj() {
    proj_dir="$1"; toml_body="$2"; marker_name="$3"
    mkdir -p "$proj_dir"
    cat > "$proj_dir/aether.toml" <<EOF
[build]
$toml_body

[[bin]]
name = "probe"
path = "probe.ae"
extra_sources = ["shim.c"]
EOF
    cat > "$proj_dir/probe.ae" <<EOF
extern ae_env_exp_probe_value() -> int

main() {
    v = ae_env_exp_probe_value()
    if v != 42 {
        println("FAIL")
        exit(1)
    }
    println("OK")
}
EOF
    cat > "$proj_dir/shim.c" <<EOF
#ifndef $marker_name
#error "$marker_name not defined — \${VAR} expansion did not reach gcc"
#endif

int ae_env_exp_probe_value(void) { return $marker_name; }
EOF
}

# --- Test 1: allowlisted env var expands; build succeeds, probe prints OK
make_proj "$tmpdir/t1" 'cflags = "${AETHER_TEST_T1_CFLAGS}"' "AE_ENV_EXP_T1"
( cd "$tmpdir/t1" && \
  AETHER_TEST_T1_CFLAGS="-DAE_ENV_EXP_T1=42" "$AE" run probe.ae ) >"$tmpdir/t1.log" 2>&1
t1_rc=$?
if [ "$t1_rc" -ne 0 ]; then
    echo "  [FAIL] aether_toml_env_var_expansion T1: ae run rejected the project (env expansion did not reach gcc)"
    sed 's/^/    /' "$tmpdir/t1.log" | head -15
    fail=1
elif ! grep -q "^OK$" "$tmpdir/t1.log"; then
    echo "  [FAIL] aether_toml_env_var_expansion T1: probe did not print OK"
    sed 's/^/    /' "$tmpdir/t1.log" | head -10
    fail=1
else
    echo "  [PASS] aether_toml_env_var_expansion T1: \${AETHER_*} env var expanded into cflags"
fi

# --- Test 2: unset allowlisted var warns + expands empty; shim.c #error fires
make_proj "$tmpdir/t2" 'cflags = "${AETHER_TEST_T2_CFLAGS}"' "AE_ENV_EXP_T2"
( cd "$tmpdir/t2" && unset AETHER_TEST_T2_CFLAGS && "$AE" run probe.ae ) >"$tmpdir/t2.log" 2>&1
t2_rc=$?
if [ "$t2_rc" -eq 0 ]; then
    echo "  [FAIL] aether_toml_env_var_expansion T2: build succeeded with unset env var (should have failed at shim.c #error)"
    sed 's/^/    /' "$tmpdir/t2.log" | head -10
    fail=1
elif ! grep -q "AETHER_TEST_T2_CFLAGS" "$tmpdir/t2.log"; then
    echo "  [FAIL] aether_toml_env_var_expansion T2: build failed but warning naming the unset var was not emitted"
    sed 's/^/    /' "$tmpdir/t2.log" | head -10
    fail=1
elif ! grep -q "unset" "$tmpdir/t2.log"; then
    echo "  [FAIL] aether_toml_env_var_expansion T2: build failed but warning did not say 'unset'"
    sed 's/^/    /' "$tmpdir/t2.log" | head -10
    fail=1
else
    echo "  [PASS] aether_toml_env_var_expansion T2: unset \${AETHER_*} warns + expands empty"
fi

# --- Test 3: non-allowlisted name (no AETHER_ prefix) is REJECTED with a
#     warning that documents the allowlist; build fails (#error in shim).
make_proj "$tmpdir/t3" 'cflags = "${PATH}"' "AE_ENV_EXP_T3"
( cd "$tmpdir/t3" && "$AE" run probe.ae ) >"$tmpdir/t3.log" 2>&1
t3_rc=$?
if [ "$t3_rc" -eq 0 ]; then
    echo "  [FAIL] aether_toml_env_var_expansion T3: non-allowlisted \${PATH} was honoured (allowlist bypass)"
    fail=1
elif ! grep -q '${PATH}' "$tmpdir/t3.log" && ! grep -q 'PATH' "$tmpdir/t3.log"; then
    echo "  [FAIL] aether_toml_env_var_expansion T3: build failed but no warning mentioned PATH"
    sed 's/^/    /' "$tmpdir/t3.log" | head -10
    fail=1
elif ! grep -q "AETHER_\*" "$tmpdir/t3.log"; then
    echo "  [FAIL] aether_toml_env_var_expansion T3: build failed but warning did not document the allowlist"
    sed 's/^/    /' "$tmpdir/t3.log" | head -10
    fail=1
else
    echo "  [PASS] aether_toml_env_var_expansion T3: non-allowlisted \${X} blocked + warned"
fi

# Escape (\$) is intentionally not covered as integration — it's a
# helper-internal grammar concern (no real user-visible cflags shape
# needs literal $). The unit-test surface in the helper would be the
# better home for it.

exit $fail
