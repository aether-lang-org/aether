#!/bin/bash
# Install Aether Language Support for VS Code / Cursor (Linux/macOS).
# Run from anywhere: `./editor/vscode/install.sh` (paths resolved from
# this script's own directory).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Read the version straight from package.json so the installed-folder
# name tracks the manifest. Avoids the trap of bumping package.json's
# `version` and forgetting to update a hardcoded EXT_NAME here, which
# is what made earlier installs ship without the themes/ payload.
VERSION="$(awk -F'"' '/"version"/ {print $4; exit}' "$SCRIPT_DIR/package.json")"
if [ -z "$VERSION" ]; then
    echo "Error: failed to read version from package.json" >&2
    exit 1
fi
EXT_BASENAME="aether-language"
EXT_DIR_NAME="${EXT_BASENAME}-${VERSION}"

# Every file VS Code needs to load the language, grammar, theme, and
# icons. Adding a new asset (e.g. a contrib/ snippet file)? Add it
# here too — silently skipping it shipped a broken extension before.
copy_extension_assets() {
    local target_dir="$1"
    mkdir -p "$target_dir/out"

    # Manifest + grammar + language config (always required).
    cp "$SCRIPT_DIR/package.json"                 "$target_dir/"
    cp "$SCRIPT_DIR/aether.tmLanguage.json"       "$target_dir/"
    cp "$SCRIPT_DIR/language-configuration.json"  "$target_dir/"

    # File-icon theme — gives `.ae` files the module icon in the
    # explorer pane. Distinct from a *colour* theme; the extension
    # no longer ships one of those (the user's chosen colour theme
    # paints the TextMate scopes from the grammar above).
    cp "$SCRIPT_DIR/aether-icon-theme.json"       "$target_dir/"

    # Icons.
    cp "$SCRIPT_DIR/icon-module.svg"              "$target_dir/"
    cp "$SCRIPT_DIR/icon.png"                     "$target_dir/"

    # README — VS Code's extension panel displays this when the user
    # clicks the extension entry.
    [ -f "$SCRIPT_DIR/README.md" ] && cp "$SCRIPT_DIR/README.md" "$target_dir/"

    # LSP client bundle. The repo ships a pre-built `out/extension.js`
    # so end users don't need node/npm to install. If the bundle is
    # missing (someone wiped `out/` without rebuilding) we still copy
    # everything else and warn — the extension then falls back to
    # syntax-only mode, which is degraded but not broken.
    if [ -f "$SCRIPT_DIR/out/extension.js" ]; then
        cp "$SCRIPT_DIR/out/extension.js" "$target_dir/out/"
    else
        echo "  Warning: out/extension.js not found; LSP client won't auto-start." >&2
        echo "           Build it with: cd $SCRIPT_DIR && npm install && npm run build" >&2
    fi
}

install_extension() {
    local extensions_root="$1"
    local editor_name="$2"

    echo "Installing Aether language support v${VERSION} for ${editor_name}..."
    echo "Target: ${extensions_root}/${EXT_DIR_NAME}"

    # Remove any prior install of the same extension family (any
    # version) so stale assets from older releases don't shadow new
    # ones. Three naming patterns to catch:
    #   1. `aether-language-<version>` — folders this script produces.
    #   2. `aether.aether-language-<version>` — folders the VS Code
    #      Marketplace produces (publisher-id prefixed) when the
    #      publisher is `aether`.
    #   3. `aether-lang.aether-language-<version>` — leftover from
    #      an earlier publisher-id rename (`aether-lang` → `aether`).
    #      These shadowed the new install and broke activation:
    #      VS Code saw two folders both registering language id
    #      `aether` + scope `source.aether` and the older one won.
    # Bounded to the Aether-language basename — never touch unrelated
    # extension folders.
    if [ -d "$extensions_root" ]; then
        find "$extensions_root" -maxdepth 1 -type d \
            \( -name "${EXT_BASENAME}-*" \
            -o -name "aether.${EXT_BASENAME}-*" \
            -o -name "aether-lang.${EXT_BASENAME}-*" \) \
            -exec rm -rf {} + 2>/dev/null || true
    fi

    copy_extension_assets "${extensions_root}/${EXT_DIR_NAME}"

    # Heal the editor's own extension-manifest state. VS Code and
    # Cursor maintain TWO JSON files alongside the per-extension
    # directories:
    #
    #   extensions.json   — array of registered extensions, each
    #                       entry naming its on-disk directory.
    #                       The editor reads this on startup AND
    #                       trusts it; if it points at a folder
    #                       that no longer exists, the editor
    #                       throws "Unable to read file
    #                       <ext>/package.json" on every .ae open.
    #   .obsolete         — JSON object whose keys are
    #                       `<publisher>.<name>-<version>` strings
    #                       the editor was told are obsolete and
    #                       should be SKIPPED at load time. If our
    #                       new version appears here (e.g. because
    #                       a prior install was uninstalled via
    #                       the editor's UI), the freshly-copied
    #                       extension is invisible.
    #
    # Stripping every `aether*` entry from BOTH files forces the
    # editor to re-scan the directory on next startup and re-register
    # what it actually finds. Targeted to our publisher prefix; never
    # touches unrelated entries. Python (which both editors ship in
    # MSYS2 / macOS / typical Linux installs) is the safest cross-
    # platform JSON editor — `jq` isn't universal, sed-on-JSON is
    # fragile. Falls back silently if Python isn't present (the
    # filesystem cleanup above still runs).
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$extensions_root" <<'PYEOF'
import json, sys, pathlib
root = pathlib.Path(sys.argv[1])

# extensions.json — drop entries whose id or location mentions
# 'aether'. Cursor re-discovers the live folder on next startup.
mpath = root / 'extensions.json'
if mpath.exists():
    try:
        data = json.loads(mpath.read_text())
        if isinstance(data, list):
            cleaned = [e for e in data
                       if 'aether' not in e.get('identifier', {}).get('id', '').lower()
                       and 'aether' not in e.get('location', {}).get('path', '').lower()]
            if len(cleaned) != len(data):
                mpath.write_text(json.dumps(cleaned) + '\n')
    except (json.JSONDecodeError, OSError):
        pass  # Editor will regenerate it; not our concern.

# .obsolete — drop aether keys so our fresh install isn't skipped.
opath = root / '.obsolete'
if opath.exists():
    try:
        data = json.loads(opath.read_text())
        if isinstance(data, dict):
            cleaned = {k: v for k, v in data.items() if 'aether' not in k.lower()}
            if len(cleaned) != len(data):
                opath.write_text(json.dumps(cleaned))
    except (json.JSONDecodeError, OSError):
        pass
PYEOF
    fi

    echo "✓ Extension installed successfully."
    echo "  Restart ${editor_name} for the language to register."
    echo "  .ae files use a fixed palette regardless of your theme;"
    echo "  every other file type still uses your active colour theme."
    echo "  LSP:   ensure 'aether-lsp' is on PATH (or set 'aether.lsp.path')."
}

# Resolve install target. Both editors keep their extensions under
# ~/.<editor>/extensions/. Cursor wins if both are installed, since it
# inherits VS Code's extension protocol and is what most Aether devs
# run today; pass an explicit override via $1 to install elsewhere
# (root install.sh delegates here with the editor's extensions dir).
TARGET_OVERRIDE="${1:-}"
if [ -n "$TARGET_OVERRIDE" ]; then
    # Infer the editor name from the override path so the success
    # message reads "Restart Cursor" / "Restart VS Code" rather than
    # the generic "Restart custom path" the script used to print.
    case "$TARGET_OVERRIDE" in
        *.cursor/extensions*)        target_label="Cursor" ;;
        *.vscode/extensions*)        target_label="VS Code" ;;
        *.vscode-insiders/extensions*) target_label="VS Code Insiders" ;;
        *.cursor-server/extensions*) target_label="Cursor (remote)" ;;
        *)                           target_label="your editor" ;;
    esac
    install_extension "$TARGET_OVERRIDE" "$target_label"
elif [ -d "$HOME/.cursor/extensions" ]; then
    install_extension "$HOME/.cursor/extensions" "Cursor"
elif [ -d "$HOME/.vscode/extensions" ]; then
    install_extension "$HOME/.vscode/extensions" "VS Code"
else
    echo "Error: neither VS Code nor Cursor extensions directory found." >&2
    echo "       Tried: ~/.cursor/extensions, ~/.vscode/extensions"      >&2
    echo "       Override with: $0 /path/to/extensions"                  >&2
    exit 1
fi
