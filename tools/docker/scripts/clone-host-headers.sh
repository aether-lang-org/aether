#!/bin/sh
# Clone the hosted-language-headers repo at a pinned commit. The
# per-language WITH_<LANG> scripts then just copy from
# /opt/hosted-language-headers/<lang>/ into /opt/aether/include/<lang>/.
# This replaces the apt-install-dev / capture / apt-purge dance with
# a single network fetch + flat copies.
#
# Why pinned: reproducible builds. If hosted-language-headers gets
# new headers for a different language version (Python 3.12, say),
# image builds keep using the version we tested against until the
# pin is bumped explicitly.
#
# Why git rather than a tarball: cheap branching for non-x86_64 /
# non-glibc targets later. Today main = linux-x86_64-glibc; a
# linux-arm64-glibc branch would land later without a release-
# artefact dance.

set -eu

HEADERS_REPO="${HEADERS_REPO:-https://github.com/aether-lang-org/hosted-language-headers.git}"
HEADERS_REF="${HEADERS_REF:-6872b5df70b3098387a2ca44839e67293c14d54a}"
HEADERS_DIR="${HEADERS_DIR:-/opt/hosted-language-headers}"

# Skip if already present (e.g. cached layer from a previous build).
if [ -d "$HEADERS_DIR/.git" ]; then
    cd "$HEADERS_DIR"
    if [ "$(git rev-parse HEAD)" = "$HEADERS_REF" ]; then
        echo "clone-host-headers: $HEADERS_DIR already at $HEADERS_REF, skipping"
        exit 0
    fi
fi

# Shallow clone to keep image-layer size minimal. Then check out
# the pinned commit. --filter=blob:none would be even smaller but
# requires git 2.19+ which bookworm-bookworm has (2.39) — keep
# this simple.
mkdir -p "$(dirname "$HEADERS_DIR")"
rm -rf "$HEADERS_DIR"
git clone --depth 1 "$HEADERS_REPO" "$HEADERS_DIR"
cd "$HEADERS_DIR"

# `--depth 1` lands us on the current HEAD of the default branch.
# If that's not the pinned commit, deepen the clone and check out.
if [ "$(git rev-parse HEAD)" != "$HEADERS_REF" ]; then
    git fetch --depth 1 origin "$HEADERS_REF"
    git checkout "$HEADERS_REF"
fi

# Drop .git to save layer size — we won't be running git operations
# against the repo again from inside the image.
rm -rf "$HEADERS_DIR/.git"

echo "clone-host-headers: pinned at $HEADERS_REF in $HEADERS_DIR ($(du -sh "$HEADERS_DIR" | cut -f1))"
