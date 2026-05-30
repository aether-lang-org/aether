#!/bin/sh
# Duration values must not compare against plain numbers silently.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

cat > /tmp/ae_duration_bad.ae << 'EOF'
main() {
    if 5s > 5 {
        println("bad")
    }
}
EOF

output=$(AETHER_HOME="" "$ROOT/build/ae" build /tmp/ae_duration_bad.ae -o /tmp/ae_duration_bad_out 2>&1)
rm -f /tmp/ae_duration_bad.ae /tmp/ae_duration_bad_out

if echo "$output" | grep -q "Invalid operation for given types"; then
    echo "duration_type_error: PASS"
    exit 0
fi

echo "duration_type_error: FAIL"
echo "$output" | head -20 | sed 's/^/  /'
exit 1
