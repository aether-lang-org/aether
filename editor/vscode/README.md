# Aether Language Support

Visual Studio Code (and Cursor) support for the
[Aether programming language](https://github.com/nicolasmd87/aether).

## What you get

- **Syntax highlighting** for `.ae` files, scoped specifically for actor
  definitions, message types, struct types, pattern arrows (`->`),
  cons-cell patterns (`[h | t]`), and the actor send / ask operators
  (`!`, `?`).
- **Fixed palette** applied to `.ae` files regardless of your active
  VS Code colour theme. Designed for dark-theme readability and
  semantic legibility — each token type gets a colour that reflects
  its role in the language:

  | Token | Colour | Style |
  |---|---|---|
  | Comments (`//`, `/* */`) | `#6A9955` (forest green) | italic |
  | Strings | `#CE9178` (warm rose) | — |
  | Numbers | `#B5CEA8` (sage) | — |
  | `true` / `false` / `null` | `#4EC9B0` (teal) | italic |
  | Primitive types (`int`, `string`, …) | `#4EC9B0` (teal) | — |
  | Actor / message names | `#4EC9B0` (teal) | **bold** |
  | Struct / type names | `#4EC9B0` (teal) | — |
  | Keywords (`if`, `else`, `match`, …) | `#C586C0` (violet) | italic |
  | Storage (`struct`, `actor`, `fn`) | `#C586C0` (violet) | italic |
  | Function definitions | `#DCDCAA` (warm yellow) | **bold** |
  | `main()` | `#DCDCAA` | **bold italic** |
  | Built-in / stdlib calls | `#DCDCAA` | — |
  | Variables | `#9CDCFE` (sky blue) | — |
  | Arrows / actor ops (`->`, `<-`, `!`, `::`) | `#D19A66` (pumpkin) | **bold** |
  | Other operators | `#D4D4D4` (light gray) | — |
  | Brackets, accessors (`.`, `[]`) | `#808080` (mid gray) | — |
  | String interpolation `${…}` | `#C586C0` (violet) | — |
  | String escapes (`\n`, `\\`, …) | `#D7BA7D` (mustard) | — |

  The override is bounded to `.aether`-suffixed scopes only —
  anything else in your workspace (Markdown, JSON, JS, …) keeps
  your active theme's colours.
- **File icon** — a yellow-on-dark "ae" badge for `.ae` files.
- **Language configuration** — auto-closing brackets, comment toggle,
  smart indent.
- **Language Server (LSP) auto-wired** — when you open a `.ae` file the
  extension finds your `aether-lsp` binary and starts a language
  client against it. No manual configuration: it tries
  `aether.lsp.path`, then the workspace's `build/aether-lsp`, then
  `PATH`, then common install dirs (`~/.local/bin`, `~/.aether/bin`,
  `/usr/local/bin`, `/opt/homebrew/bin`). Set
  `aether.lsp.enable: false` for syntax-only mode.

## Activating the palette

The palette applies automatically the moment a `.ae` file opens —
no theme switch needed. The extension ships its colours via
`editor.tokenColorCustomizations.textMateRules` scoped to
`*.aether`, which VS Code applies on top of whatever theme is
already active.

To verify it's working: open any `.ae` file and check that comments
are green-italic, keywords (`fn`, `if`, `match`) are violet-italic,
and function names are warm yellow bold. If you see your theme's
default colours instead, restart VS Code (extensions are loaded once
per session).

## Installation

### From source

```bash
./editor/vscode/install.sh
```

The script:

- Reads the version straight from `package.json` so the installed
  folder name tracks the manifest.
- Removes any prior `aether-language-*` directory before copying so
  stale assets from older releases can't shadow the new ones.
- Copies the full asset set (manifest, grammar, language config,
  theme, icon-theme, both icon files, README) into
  `~/.cursor/extensions/` (Cursor) or `~/.vscode/extensions/`
  (VS Code).
- Supports an explicit override target:
  `./install.sh /path/to/extensions`.

Restart your editor after installing.

### From a `.vsix`

Standard `vsce package` workflow if you've installed `vsce` (a
follow-up will publish official releases to the marketplace; for
now the install script is the supported path).

## Language Server Protocol

The extension auto-starts an LSP client on any `.ae` file open. The
language server is now embedded in `aetherc` itself (issue #327, the
`aetherc lsp` subcommand) so a single toolchain binary is enough —
the standalone `aether-lsp` is kept as a transitional alias for
editor configs that hardcode the older name.

The resolver tries each location in order, preferring `aetherc lsp`
over `aether-lsp` when both are present:

1. The `aether.lsp.path` setting if you've set it.
2. `<workspace>/build/aetherc lsp` (or `aether-lsp` as fallback) —
   the common case for working in the Aether repo itself; just
   `make compiler` and the extension finds it.
3. `aetherc lsp` (then `aether-lsp`) resolved through your shell
   `PATH`.
4. Common install directories: `~/.local/bin`, `~/.aether/bin`,
   `/usr/local/bin`, `/opt/homebrew/bin`.

If none of those find an executable, you'll see a one-time warning
with a link to the setting; the extension stays in syntax-only mode
until you provide a path or build the server. See
[`lsp/README.md`](../../lsp/README.md) for what the server currently
supports.

## Building the extension client from source (maintainers)

The bundled `out/extension.js` is committed so end users don't need
node. If you change `src/extension.ts`, regenerate it with:

```bash
cd editor/vscode
npm install
npm run build      # esbuild bundle to out/extension.js
npm run typecheck  # tsc --noEmit, sanity check
```

## Example

```aether
import std.string

message Tick { count: int }

actor Heartbeat {
    state ticks = 0

    receive {
        Tick(count) -> {
            ticks = ticks + 1
            if ticks % 10 == 0 {
                println("heartbeat ${ticks}")
            }
        }
    }
}

main() {
    h = spawn(Heartbeat())
    i = 0
    while i < 100 {
        h ! Tick { count: i }
        i = i + 1
    }
}
```

## Requirements

- Visual Studio Code 1.60.0+ (or Cursor on the same protocol level)

## Reporting issues

Open an issue at
[github.com/nicolasmd87/aether/issues](https://github.com/nicolasmd87/aether/issues).

## License

MIT — see the [LICENSE](../../LICENSE) at the repo root.
