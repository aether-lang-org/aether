#!/bin/sh
# See install-python-headers.sh for the pattern.

set -eu

HEADERS_DIR="${HEADERS_DIR:-/opt/hosted-language-headers}"

mkdir -p /opt/aether/include
cp -r "$HEADERS_DIR/lua" /opt/aether/include/lua5.4

test -f /opt/aether/include/lua5.4/lua.h || {
    echo "install-lua-headers: lua.h missing" >&2
    exit 1
}

apt-get update
apt-get install -y --no-install-recommends liblua5.4-0
rm -rf /var/lib/apt/lists/*

echo "install-lua-headers: $(du -sh /opt/aether/include/lua5.4 | cut -f1) at /opt/aether/include/lua5.4"
