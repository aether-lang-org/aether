#!/bin/sh
# Issue #586: bare-int → Duration at parameter-passing sites must
# be a compile-time error. Verifies the typechecker error fires
# with the unit-suffix suggestion message.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

cat > /tmp/ae_dur_param_bad.ae << 'EOF'
set_timeout(t: Duration) -> int {
    return 0
}

main() {
    set_timeout(5)
}
EOF

output=$(AETHER_HOME="" "$ROOT/build/ae" build /tmp/ae_dur_param_bad.ae \
            -o /tmp/ae_dur_param_bad_out 2>&1)
rm -f /tmp/ae_dur_param_bad.ae /tmp/ae_dur_param_bad_out

# Must reject with the Duration-strictness diagnostic and mention
# at least one unit-suffix form in the suggestion.
if ! echo "$output" | grep -q "Cannot pass int where Duration expected"; then
    echo "duration_param_strict_error: FAIL (no Duration mismatch error)"
    echo "$output" | head -20 | sed 's/^/  /'
    exit 1
fi

if ! echo "$output" | grep -q "5s"; then
    echo "duration_param_strict_error: FAIL (no unit-suffix suggestion)"
    echo "$output" | head -20 | sed 's/^/  /'
    exit 1
fi

echo "duration_param_strict_error: PASS"
exit 0
