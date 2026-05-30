#!/bin/sh
# Regression: contrib.xml.expat — SAX-style streaming XML parser
# veneer over libexpat. See probe.ae for the per-case matrix.
#
# libexpat isn't auto-detected by the Aether toolchain. We probe for
# it via pkg-config (with a gcc-link fallback for systems where the
# pkg-config metadata is absent) and SKIP the test cleanly when
# absent. Mirrors the sqlite_roundtrip pattern; same rationale.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

# -- Capability probe -----------------------------------------------------
probe_expat_available() {
    if pkg-config --exists expat 2>/dev/null; then
        return 0
    fi
    # Last-ditch: try a direct compile in case pkg-config metadata is absent
    # but the libexpat-dev package is installed.
    probe_c="$TMPDIR/probe_expat.c"
    cat > "$probe_c" <<'EOF'
#include <expat.h>
int main(void) { XML_Parser p = XML_ParserCreate(0); XML_ParserFree(p); return 0; }
EOF
    gcc "$probe_c" -lexpat -o "$TMPDIR/probe_expat" >/dev/null 2>&1
}

if ! probe_expat_available; then
    echo "  [PASS] contrib_xml_expat: SKIP (libexpat not installed)"
    exit 0
fi

# -- Build ---------------------------------------------------------------
# Stage a workspace with its own aether.toml so the -lexpat link flag
# flows into `ae build`'s gcc invocation via get_link_flags(). The
# workspace symlinks contrib/ so `import contrib.xml.expat` resolves.
WORK="$TMPDIR/work"
mkdir -p "$WORK"
ln -s "$ROOT/contrib" "$WORK/contrib"
cp "$SCRIPT_DIR/probe.ae" "$WORK/probe.ae"

cat > "$WORK/aether.toml" <<EOF
[project]
name = "xml_probe"
version = "0.0.0"

[[bin]]
name = "probe"
path = "probe.ae"
extra_sources = ["contrib/xml/expat/aether_xml_expat.c"]

[build]
link_flags = "-lexpat"
EOF

if ! ( cd "$WORK" && "$ROOT/build/ae" build "probe.ae" -o "$TMPDIR/probe" \
        >"$TMPDIR/build.log" 2>&1 ); then
    echo "  [FAIL] contrib_xml_expat: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -30
    exit 1
fi

# `ae build` is known to exit 0 even when the gcc link fails, so verify
# the binary was actually produced before trying to run it.
if [ ! -x "$TMPDIR/probe" ]; then
    echo "  [FAIL] contrib_xml_expat: build produced no binary"
    sed 's/^/    /' "$TMPDIR/build.log" | head -30
    exit 1
fi

# -- Run -----------------------------------------------------------------
if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] contrib_xml_expat: probe exited non-zero"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

if ! grep -q "All contrib.xml.expat tests passed" "$TMPDIR/run.log"; then
    echo "  [FAIL] contrib_xml_expat: didn't reach final PASS line"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

echo "  [PASS] contrib_xml_expat: 8 cases"
