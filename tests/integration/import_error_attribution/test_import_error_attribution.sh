#!/bin/sh
# Regression (#646): a parse error originating in an IMPORTED module must
# be attributed to that module's file and rendered from that module's
# source — not the importing file's. Pre-fix, the diagnostic named the
# importing file (main.ae) and used the imported file's line number to
# index into main.ae's buffer, printing an unrelated (valid) source line
# with a meaningless caret.
#
# Acceptance:
#   1. an error IS reported,
#   2. its `-->` location names the imported module (module.ae),
#   3. NO `-->` location names the importing file (main.ae),
#   4. the rendered snippet shows the broken module line
#      (module_orphan_field) and NOT the importer's marker
#      (importer_valid_marker).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

fail=0
tmpdir="$(mktemp -d)"
log="$tmpdir/cc.log"

# aetherc prints parse errors to stderr (and currently exits 0); inspect
# the log rather than the return code.
"$AETHERC" "$SCRIPT_DIR/main.ae" "$tmpdir/out.c" >"$log" 2>&1

if ! grep -q '^error' "$log"; then
    echo "  [FAIL] import_error_attribution: no error reported for a broken imported module"
    sed 's/^/          /' "$log" | head -8
    fail=1
fi

# (2) the location header must name the imported module
if ! grep -E '^[[:space:]]*-->' "$log" | grep -q 'module\.ae'; then
    echo "  [FAIL] import_error_attribution: diagnostic location does not name the imported module.ae"
    echo "        got location lines:"
    grep -E '^[[:space:]]*-->' "$log" | sed 's/^/          /' | head -4
    fail=1
fi

# (3) NO location header may name the importing file
if grep -E '^[[:space:]]*-->' "$log" | grep -q 'main\.ae'; then
    echo "  [FAIL] import_error_attribution: diagnostic wrongly attributes the error to the importing main.ae"
    grep -E '^[[:space:]]*-->' "$log" | sed 's/^/          /' | head -4
    fail=1
fi

# (4) the snippet must show the broken module line, not the importer's
if ! grep -q 'module_orphan_field' "$log"; then
    echo "  [FAIL] import_error_attribution: snippet does not render the broken module line"
    sed 's/^/          /' "$log" | head -10
    fail=1
fi
if grep -q 'importer_valid_marker' "$log"; then
    echo "  [FAIL] import_error_attribution: snippet rendered the importer's source (wrong buffer)"
    sed 's/^/          /' "$log" | head -10
    fail=1
fi

rm -rf "$tmpdir"
if [ "$fail" -eq 0 ]; then
    echo "  [PASS] import_error_attribution: imported-module parse error attributed to module.ae"
fi
exit $fail
