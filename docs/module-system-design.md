# Aether Module System

This document describes Aether's module and namespace system.

## Terminology

| Term | Definition | Example |
|------|------------|---------|
| **Package** | A collection of related modules | `std` (standard library) |
| **Module** | A single importable unit with functions | `std.string`, `std.http` |
| **Namespace** | The prefix used when calling functions | `string`, `http` |

## Current Implementation

The standard library uses **namespace-style function calls**:

```aether
import std.string    // Import the string module
import std.file      // Import the file module

main() {
    // Namespace = module name after last dot
    s = string.new("hello")     // string namespace
    len = string.length(s)
    string.release(s)

    if (file.exists("data.txt") == 1) {   // file namespace
        size = file.size("data.txt")
    }
}
```

### How It Works

1. `import std.X` registers `X` as a namespace
2. `X.func()` is resolved to the C function `X_func()`
3. Dot-style calls: `string.new()`, `list.free()`, etc. (compiler translates to C-level `string_new`, `list_free`)

### Module Orchestration Phase

Before type checking, the compiler runs a module orchestration phase
(added between parsing and type checking in the compilation pipeline):

1. **Scan**: Walk the main program AST for `import` statements
2. **Resolve**: Map each import to a file path (stdlib, lib/, src/ paths)
3. **Parse**: Lex and parse each module file into an AST
4. **Cache**: Store the parsed AST in `global_module_registry`
5. **Recurse**: Process each module's own imports (transitive dependencies)
6. **Cycle Check**: Build a dependency graph and detect circular imports via DFS

Both the type checker and code generator then use `module_find()` to retrieve
cached ASTs — each module file is read and parsed exactly once.

### Available Modules

| Import | Namespace | Functions |
|--------|-----------|-----------|
| `import std.string` | `string` | `string.new()`, `string.length()`, `string.release()` |
| `import std.file` | `file` | `file.open()`, `file.read_all()`, `file.write()`, `file.exists()`, `file.close()`, `file.delete()`, `file.size()` |
| `import std.dir` | `dir` | `dir.exists()`, `dir.create()`, `dir.delete()`, `dir.list()` |
| `import std.path` | `path` | `path.join()`, `path.dirname()`, `path.basename()`, `path.extension()`, `path.is_absolute()` |
| `import std.fs` | `file`, `dir`, `path` | Combined module — re-exports `file.*`, `dir.*`, and `path.*` operations |
| `import std.json` | `json` | `json.parse()`, `json.create_object()`, `json.free()` |
| `import std.http` | `http` | `http.get()`, `http.server_create()`, `http.server_start()` |
| `import std.tcp` | `tcp` | `tcp.connect()`, `tcp.write()`, `tcp.listen()` |
| `import std.net` | `tcp`, `http` | Combined module — re-exports `tcp.*` and `http.*` operations |
| `import std.list` | `list` | `list.new()`, `list.add()`, `list.get()`, `list.set()`, `list.remove()` |
| `import std.map` | `map` | `map.new()`, `map.put()`, `map.get()`, `map.has()`, `map.remove()` |
| `import std.collections` | `list`, `map` | Combined module — re-exports `list.*` and `map.*` operations |
| `import std.math` | `math` | `math.abs_int()`, `math.sqrt()`, `math.sin()`, `math.random_int()` |
| `import std.log` | `log` | `log.init()`, `log.write()`, `log.shutdown()` |
| `import std.io` | `io` | `io.print()`, `io.read_file()`, `io.getenv()` |
| `import std.os` | `os` | `os.system()`, `os.exec()`, `os.getenv()` |

---

## Export Visibility

Modules declare their public API via a single Erlang-style `exports (…)`
list at the top of the file. Names in the list are accessible from
importers; names not in the list are private — still usable inside the
module's own functions, but rejected at qualified-call sites.

```aether
// lib/geometry/module.ae

exports (PI, distance)

const PI = 3

distance(x1, y1, x2, y2) {
    dx = x1 - x2
    dy = y1 - y2
    return _sqrt_approx(dx * dx + dy * dy)
}

// Private helper — not in the exports list, not accessible from outside.
_sqrt_approx(n) {
    return n  // placeholder
}
```

```aether
import geometry

main() {
    d = geometry.distance(0, 0, 3, 4)   // OK — listed
    println(geometry.PI)                 // OK — listed
    // geometry._sqrt_approx(25)         // Error: not in exports list
}
```

**Rules:**
- `exports (…)` accepts any combination of function names, const names,
  and struct/actor names. The list is the contract.
- A module that declares an `exports (…)` list exposes **only** the
  listed names. Anything else is private.
- A module with no `exports (…)` list keeps the legacy default-public
  behavior — every top-level name is callable from outside. v1 is
  additive; v2 will flip this default to private once every stdlib +
  contrib module has been migrated.
- Private names can still be referenced from inside the module's own
  functions — they're merged into the program, just blocked at the
  external call boundary.

### Legacy `export <fn>` form (deprecated)

Earlier versions of Aether used a per-function `export` keyword:

```aether
// DEPRECATED — emits a warning at compile time.
export const PI = 3
export distance(...) { … }
```

This form is still accepted for one release as a deprecation alias.
Modules using it get a one-shot warning to stderr. Migrate by
collecting every `export`-tagged name into one `exports (…)` line at
the top, then removing the per-function keywords. Mixing both forms in
one module is a hard error.

## Selective-import shadow rejection (#436 facet A)

A module (or main program) that selectively imports a name AND defines a local function with the same name silently shadowed the import:

```aether
import std.string (length)

length(s: string) -> int {
    return length(s)        // intent: std.string.length
                            // reality: self → infinite recursion
}
```

Pre-fix, the merger renamed the body's bare `length(s)` to `mod_length(s)` (matching the local def's namespaced name post-merge), turning the wrapper into self-recursion — a runtime stack overflow with no compile-time signal. As of #436 facet A, the orchestrator rejects this pattern with `error[E1000]` before merging:

```
error[E1000]: module 'mod' (mod/module.ae) defines local function
'length' (3:1) but also selectively imports 'length' from 'std.string'
  the local def silently shadows the import, so a bare call to
  'length(...)' inside the local body would recurse into the local
  rather than forward to the imported symbol — at runtime this is a
  stack overflow with no compile-time signal.
  fix one of:
    - rename the local function
    - drop the selective import: `import std.string` (then call via
      the qualified form)
    - keep both but call the imported version qualified inside the
      local body
```

The same check applies to entry-point `main.ae` files when the shadow lives there directly (not inside a module). Both `AST_FUNCTION_DEFINITION` and `export <fn>` shapes are caught.

## Qualified surface survives a selective import after merge (#870)

The two `import std.X` forms each light up a different call syntax: a bare
`import std.X` enables the qualified `X.fn()` surface; a selective
`import std.X (fn)` enables the bare-name `fn()` surface. These compose
per-file — but a merged compilation unit pulls definitions from several files
into one program, and a module's own `import` statements are *not* cloned in
(only its function/const/struct bodies are).

That dropped the qualified surface in one case: when the entry file imported a
module **selectively** (`import std.string (string_length)`), the merged unit
was marked selective for `string`, so a qualified `string.concat(...)` arriving
from an imported module that bare-imports `std.string` was rejected with
`error[E0301]: Undefined function 'string.concat'`.

The merge now injects a **synthetic bare import** for each merged module's own
bare imports. A synthetic import marks the namespace non-selective for the unit
(re-opening `string.X`) but is kept out of the user-explicit registry, so
sealed-scope isolation (#243) is preserved: user code still cannot call into a
namespace it never imported. This mirrors how transitive dependencies already
get synthetic imports during merge.

## Future

Work planned on top of the current module system:

- `import math.geometry as geo` — import aliases (parser accepts them; wiring through the typechecker and symbol table is next).
- Exporting structs and actors from modules.
- Re-exports (module A re-exporting module B's symbols).
- Transitive dependency resolution.

---

## Creating Packages

Developers can create their own packages using the `ae` CLI tool.

### Quick Start

```bash
# Create a new package
ae init mypackage
cd mypackage

# Build and run
ae build
ae run
```

### Package Structure

```
mypackage/
├── aether.toml       # Package manifest
├── src/
│   └── main.ae       # Main entry point
├── lib/              # Library modules (optional)
│   └── utils/
│       └── module.ae # import utils → lib/utils/module.ae
├── tests/            # Test files (optional)
│   └── test_utils.ae
└── README.md
```

### Package Manifest (aether.toml)

Every package has an `aether.toml` file:

```toml
[package]
name = "mypackage"
version = "0.1.0"
authors = ["Your Name <you@example.com>"]
license = "MIT"
description = "My awesome Aether package"

[dependencies]
# Add with: ae add github.com/user/repo
# github.com/user/mylib = "v1.0.0"

[dev-dependencies]
# Test dependencies

[build]
target = "native"
optimizations = "aggressive"

[[bin]]
name = "mypackage"
path = "src/main.ae"
```

### Creating Local Modules

Aether supports local modules within your project:

**Project Structure:**
```
myapp/
├── aether.toml
├── src/
│   └── main.ae              # Main entry point
├── lib/
│   └── utils/
│       └── module.ae        # Pure Aether module
└── tests/
```

**lib/utils/module.ae** — define your module:
```aether
export const MULTIPLIER = 2

export double_value(x) {
    return multiply(x, MULTIPLIER)
}

export triple_value(x) {
    return multiply(x, 3)
}

// Private helper
multiply(a, b) {
    return a * b
}
```

**src/main.ae** — use the module:
```aether
import utils

main() {
    println(utils.double_value(5))   // 10
    println(utils.triple_value(5))   // 15
    println(utils.MULTIPLIER)        // 2
}
```

### Module Resolution

The compiler searches for local modules in this order:

1. `<lib-path-entry>/<module>/module.ae` - Library directory with module file
2. `<lib-path-entry>/<module>.ae` - Single-file module in lib
3. `src/<module>/module.ae` - Source directory with module file
4. `src/<module>.ae` - Single-file module in src
5. `<module>/module.ae` - Project root
6. `<module>.ae` - Single file in project root

For nested modules like `import mypackage.utils`:
- Dots are converted to slashes: `mypackage/utils/module.ae`
- Same search paths are used with the converted path

7. `~/.aether/packages/<name>/src/module.ae` - Installed package (via `ae add`)
8. `~/.aether/packages/<name>/module.ae` - Installed package (flat layout)

Packages installed with `ae add` are searched after local paths. The package name is the first component of the import path.

#### Lib search path (#413)

`<lib-path-entry>` in steps 1 and 2 is a list of directories — PATH-style, `:` on POSIX and `;` on Windows. The list is searched left-to-right; the first hit wins. Three forms compose:

```sh
ae run main.ae --lib ./lib:./vendor/stdlib    # separator-string
ae run main.ae --lib ./lib --lib ./vendor     # repeated flag
AETHER_LIB_DIR=./lib:./vendor ae run main.ae  # env var
```

Both forms can be mixed (`--lib a:b --lib c` → `[a, b, c]`). The default is the single entry `lib` (the legacy behaviour); supplying any explicit `--lib` or `AETHER_LIB_DIR` replaces the default with the given list. Up to 8 entries. Caching invalidates on lib-path changes — two builds of the same source with different `--lib` chains get distinct cache slots, and every entry's mtime feeds into the cache key so a `touch` in a vendored module reuses the right cache bucket.

The shape matches Java `-cp`, Python `PYTHONPATH`, Ruby `-I` / `RUBYLIB`. Useful for layering project-local modules over a vendored stdlib, sharing a common lib root across two projects, or pinning a stdlib snapshot alongside HEAD-tracking deps.

Trailing slashes are normalised: `--lib ./lib/` and `--lib ./lib` register the same entry (the dedup catches them), and lookup paths stay clean — `<entry>/<module>.ae` rather than `./lib//<module>.ae`. Root paths (`/` on POSIX, `C:\` on Windows) are preserved as-is.

#### Introspecting the resolved chain

`ae lib-path` prints the resolved search order, one entry per line, in the same order the toolchain walks them. Useful when debugging "why isn't my import resolving?" without re-reading the user's shell config:

```sh
$ ae lib-path --lib ./lib:./vendor/stdlib
./lib
./vendor/stdlib

$ AETHER_LIB_DIR=/usr/share/aether:./local ae lib-path
/usr/share/aether
./local

$ ae lib-path                          # defaults — single entry
lib
```

Inputs are merged in the same priority order the build path uses: explicit `--lib` flags first, then `AETHER_LIB_DIR`, then the default `lib`.

### Namespace Convention

Function names must be prefixed with the namespace:
- Import: `import utils`
- Call: `utils.double_value(x)`
- C function: `utils_double_value()`

The compiler converts `namespace.function()` to `namespace_function()`.

### Pure Aether Modules

You can write reusable modules in pure Aether — no C backing file required:

**lib/mymath/module.ae:**
```aether
export const PI_APPROX = 3

export double_it(x) {
    return x * 2
}

export add(a, b) {
    return a + b
}

// Intra-module calls work — functions can call each other
export double_and_add(x, y) {
    return add(double_it(x), y)
}
```

**src/main.ae:**
```aether
import mymath

main() {
    println(mymath.double_it(5))    // 10
    println(mymath.add(3, 4))       // 7
    println(mymath.double_and_add(5, 3))  // 13
    println(mymath.PI_APPROX)       // 3
}
```

**How it works:**

After module orchestration, the compiler clones each module's function and constant AST nodes into the main program with namespace-prefixed names (`double_it` → `mymath_double_it`). Intra-module calls, constant references, and constant-to-constant references (e.g., `const DOUBLE_BASE = BASE * 2`) are renamed automatically. Function parameters and local variables correctly shadow module constants — `check(SCALE) { return SCALE }` returns the parameter, not the module constant `SCALE`. This makes the entire downstream pipeline (type inference, type checking, codegen) work without modification — merged functions are just regular top-level functions.

**Tree-shake of unused merges.** Immediately after merging and before typechecking, `module_prune_unreachable` runs a mark-and-sweep over the program AST. It seeds reachability from `main`, every actor handler, every `export` statement, and every non-imported user function/builder, then closes over `AST_FUNCTION_CALL`, `AST_IDENTIFIER`, and `AST_MEMBER_ACCESS` references — including suffix matches that handle glob-import (`import mymath (*)`) and selective-import (`import mymath [cube]`) shorthands. Imported function and builder definitions outside the closure are dropped from the AST so the typechecker doesn't walk them and the C compiler doesn't emit them. Constants stay (cheap, and pruning them would need a separate pass keyed on identifier references). The whole pass is invisible to user code; programs that *do* call every imported function build identically before and after.

**What's supported:**
- Functions (with type inference from call sites)
- Constants (`const NAME = value`), including constants referencing other constants
- Intra-module calls (functions calling other functions and referencing constants in the same module)
- Export visibility (`export` keyword controls public API)
- Multiple module imports in the same program
- Mixing pure modules with stdlib imports
- Parameter/local variable shadowing of module constants

**On the module-system roadmap (language-level):**
- Actors from modules (dispatch tables currently assume main-program scope)
- Message definitions from modules
- Re-exports (module A re-exporting module B's functions)
- Module-level mutable state

### Roadmap

1. **Done**: Package initialization (`ae init`)
2. **Done**: Local package building (`ae build`)
3. **Done**: Local nested package imports
4. **Done**: Pure Aether module implementations
5. **Done**: Export visibility enforcement
6. **Planned**: Package dependencies
7. **Planned**: Package registry/publishing

## Notes

- Single-file programs continue to work without packages
- The `std` package is built-in and always available
