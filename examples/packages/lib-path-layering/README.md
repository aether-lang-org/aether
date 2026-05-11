# PATH-style `--lib` layering — issue #413 demo

A self-contained workspace showing how multi-entry `--lib` resolves
module imports. The project ships `foo` in two places (`lib/foo/` and
`vendor/foo/`) and `bar` in one (`vendor/bar/`), so you can see
left-to-right resolution AND independent per-import walking.

## Layout

```
.
├── main.ae                     ← imports `foo` and `bar`
├── lib/
│   └── foo/
│       └── module.ae           ← foo_origin() → "lib/ (project-local override)"
└── vendor/
    ├── foo/
    │   └── module.ae           ← foo_origin() → "vendor/ (default)"
    └── bar/
        └── module.ae           ← bar_origin() → "vendor/ (only here)"
```

## Runs

```bash
# Project-local lib/ wins for `foo` (left-most chain entry); bar still found
# in vendor since lib/ doesn't have it. This is the typical workflow when
# you're patching a vendored dependency in-place.
$ ae run main.ae --lib lib --lib vendor
foo says: lib/ (project-local override)
bar says: vendor/ (only here)

# Same outcome with separator-string form (POSIX ':' / Windows ';'):
$ ae run main.ae --lib "lib:vendor"
foo says: lib/ (project-local override)
bar says: vendor/ (only here)

# Same outcome via env var:
$ AETHER_LIB_DIR="lib:vendor" ae run main.ae
foo says: lib/ (project-local override)
bar says: vendor/ (only here)

# Reverse the chain — vendor wins for `foo`:
$ ae run main.ae --lib vendor --lib lib
foo says: vendor/ (default)
bar says: vendor/ (only here)
```

## Debugging "why isn't my import resolving?"

`ae lib-path` prints the resolved chain in order — same parser as the
real build path, so the output is exactly what the toolchain sees:

```bash
$ ae lib-path --lib "lib:vendor"
/abs/path/to/lib
/abs/path/to/vendor

$ AETHER_LIB_DIR="vendor" ae lib-path
/abs/path/to/vendor

$ ae lib-path
lib
```

## Rules

| Rule | Why |
|------|-----|
| Left-most entry wins on a collision | Mirrors PATH / CLASSPATH / PYTHONPATH semantics — operators already know this model. |
| Each `import` walks the chain independently | A chain isn't a single root; it's a search path. `bar` not in `lib/` doesn't stop resolution from continuing to `vendor/`. |
| Trailing slash is normalised | `--lib ./lib/` and `--lib ./lib` dedup to the same entry. |
| Cap of 8 entries (`AETHER_LIB_DIRS_MAX`) | Realistic projects use 1-3; 8 gives generous headroom. Overflow warns and drops, doesn't crash. |
| Cache key includes per-entry mtime | A `touch` inside a vendored module invalidates the build cache exactly like a stdlib edit does. |
| Default chain (no flag, no env var) | A single `lib/` entry — unchanged from before #413. |

## See also

- [`docs/module-system-design.md`](../../../docs/module-system-design.md#lib-search-path-413) — full design, normalization rules, and edge cases.
