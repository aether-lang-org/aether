#!/bin/sh
# Install Python C-bridge headers by copying from the pinned
# hosted-language-headers repo (cloned earlier by
# clone-host-headers.sh). See that script for the pin rationale.
#
# We also install python3 (the runtime, NOT python3-dev) so end-user
# binaries that import contrib.host.python can dlopen libpython at
# runtime. The -dev package isn't needed any more because the
# headers come from the pinned repo.

set -eu

HEADERS_DIR="${HEADERS_DIR:-/opt/hosted-language-headers}"

# Copy headers to /opt/aether/include/python/ — the location the
# stage-2 layer copies into the runtime image.
mkdir -p /opt/aether/include
cp -r "$HEADERS_DIR/python" /opt/aether/include/python

# The configured pyconfig.h is PER-TARGET (captured from a real machine of each
# platform; removed from the shared tree — see hosted-language-headers/targets/
# README.md). This docker image is linux-x86_64-glibc, so overlay that target's
# real config. (Was previously a Debian multiarch dispatcher in the shared tree;
# the overlay is the actual configured header it used to dispatch to.)
cp "$HEADERS_DIR/targets/x86_64-linux-gnu/python/pyconfig.h" \
   /opt/aether/include/python/pyconfig.h

# Verify the contract.
test -f /opt/aether/include/python/Python.h || {
    echo "install-python-headers: Python.h missing in $HEADERS_DIR/python" >&2
    exit 1
}

# Install the Python runtime so end-user binaries can dlopen it.
apt-get update
apt-get install -y --no-install-recommends python3
rm -rf /var/lib/apt/lists/*

echo "install-python-headers: $(du -sh /opt/aether/include/python | cut -f1) at /opt/aether/include/python"
