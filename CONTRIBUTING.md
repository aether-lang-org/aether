# Contributing to Aether

Thank you for your interest in contributing to Aether. This document outlines the guidelines for contributing code, tests, and documentation.

## Code Style

### General Guidelines

- Use 2-space indentation (no tabs)
- Maximum line length: 100 characters
- Use descriptive variable and function names
- Add comments for complex logic

### Naming Conventions

```c
// Types: PascalCase
typedef struct AetherModule { ... } AetherModule;

// Functions: snake_case
void parse_expression(Parser* parser);
ASTNode* create_ast_node(ASTNodeType type);

// Variables: snake_case
int token_count = 0;
const char* module_name = "std.io";

// Constants: UPPER_SNAKE_CASE
#define MAX_TOKENS 1000
#define DEFAULT_BUFFER_SIZE 4096

// Private functions: static with leading underscore (optional)
static void _internal_helper(void);
```

### File Organization

```c
// 1. Includes (system headers first, then local)
#include <stdio.h>
#include <stdlib.h>
#include "aether_types.h"
#include "aether_parser.h"

// 2. Constants and macros
#define MAX_BUFFER 256

// 3. Type definitions
typedef struct { ... } MyStruct;

// 4. Forward declarations
static void helper_function(void);

// 5. Global variables (avoid when possible)
static int global_counter = 0;

// 6. Function implementations
void public_function(void) {
    // Implementation
}

static void helper_function(void) {
    // Implementation
}
```

### Memory Management

- Always check for `NULL` returns from allocation functions
- Free all allocated memory
- Use `defer` statement when available for automatic cleanup
- Run Valgrind to verify no memory leaks

```c
// Good
char* buffer = malloc(256);
if (!buffer) {
    return NULL;
}
// ... use buffer ...
free(buffer);

// Better (when available)
char* buffer = malloc(256);
defer(free(buffer));
if (!buffer) {
    return NULL;
}
// ... use buffer ...
// Automatically freed on scope exit
```

## Stdlib modules

When adding a new module, decide first whether it belongs in `std/`
(stability commitment, shipped with every Aether build) or `contrib/`
(opt-in, can evolve without stability constraint). The rubric is in
[docs/stdlib-vs-contrib.md](docs/stdlib-vs-contrib.md).

Once you know the placement, follow the canonical module pattern in
[docs/stdlib-module-pattern.md](docs/stdlib-module-pattern.md).

The short version: fallible C functions get a `_raw` suffix and a
Go-style `(value, err)` Aether wrapper; pure/infallible functions stay
raw without a suffix; intentionally void fire-and-forget APIs (like
`log.write`) and DSL builders (like the `std.host` manifest builders)
don't get wrappers. See [std/fs/module.ae](std/fs/module.ae) for the
reference implementation.

## Adding Tests

### Test Structure

Tests are located in the `tests/` directory and use the test harness framework.

```c
#include "test_harness.h"

// Simple test
TEST(my_feature) {
    ASSERT_EQ(add(2, 3), 5);
    ASSERT_TRUE(is_valid("test"));
}

// Test with category
TEST_CATEGORY(hashmap_insert, TEST_CATEGORY_COLLECTIONS) {
    HashMap* map = hashmap_create(16);
    ASSERT_NOT_NULL(map);
    
    hashmap_insert(map, "key", "value");
    ASSERT_STREQ(hashmap_get(map, "key"), "value");
    
    hashmap_free(map);
}
```

### Test Categories

- `TEST_CATEGORY_COMPILER` - Lexer, parser, type checker, code generator
- `TEST_CATEGORY_RUNTIME` - Actor system, scheduler, message passing
- `TEST_CATEGORY_COLLECTIONS` - HashMap, Set, Vector, PriorityQueue
- `TEST_CATEGORY_NETWORK` - HTTP, TCP, networking utilities
- `TEST_CATEGORY_MEMORY` - Arena allocators, memory pools, leak detection
- `TEST_CATEGORY_STDLIB` - Standard library functions
- `TEST_CATEGORY_PARSER` - Parser-specific tests
- `TEST_CATEGORY_OTHER` - Miscellaneous tests

### Assertion Macros

```c
ASSERT_TRUE(condition)          // Assert condition is true
ASSERT_FALSE(condition)         // Assert condition is false
ASSERT_EQ(expected, actual)     // Assert equality (integers)
ASSERT_NE(expected, actual)     // Assert inequality
ASSERT_STREQ(expected, actual)  // Assert string equality
ASSERT_STRNE(expected, actual)  // Assert string inequality
ASSERT_NULL(ptr)                // Assert pointer is NULL
ASSERT_NOT_NULL(ptr)            // Assert pointer is not NULL
```

### Running Tests

```bash
# Type-check without compiling (skips codegen + link — much faster on iteration)
ae check file.ae

# All tests
make test

# Specific category (when implemented)
./build/test_runner --category=collections

# With Valgrind
make test-valgrind

# With AddressSanitizer
make test-asan
```

## Pull Request Requirements

### Docs-only PRs: skip CI with `[skip actions]`

If a PR (or its commits) changes **only** Markdown / prose — `*.md`,
`docs/`, `CHANGELOG.md`, the README, comments-as-prose — and touches no
code, tests, build, or workflow files (`.c`, `.h`, `.ae`, `Makefile`,
`tools/`, `.github/`, `install.sh`), put **`[skip actions]`** in the
commit message. GitHub reads that token (also `[skip ci]` / `[ci skip]`)
from the branch's **HEAD commit** and skips the entire workflow run — so
a one-line doc fix doesn't spin up the dozen-runner build matrix.

```
docs: fix typo in getting-started [skip actions]
```

This conserves GitHub Actions minutes. Two cautions: it must be the HEAD
commit that carries the token (a docs-only PR with a later code commit
should NOT skip), and only use it when the change genuinely cannot
affect a build — anything under `.c` / `.ae` / `Makefile` / `tools/` /
`.github/` must run the full suite below. Skipping CI means the PR shows
no status checks, so a maintainer merges it on review alone.

### Before Submitting

Run the full CI suite locally — this is the same suite that GitHub Actions runs:

```bash
make ci          # Full 8-step suite with -Werror (compiler, tests, examples, smoke tests)
HARDEN=1 make ci # Hardened-build sweep (-fstack-protector-all + _FORTIFY_SOURCE=2);
                 # required for any PR that touches C in compiler/, runtime/, or std/.
                 # Catches unchecked memcpy / printf-format-injection bugs early.
                 # See docs/build-system.md "Hardening" for the full flag rationale.
```

This covers your current platform. To verify cross-platform compatibility:

```bash
# Cooperative scheduler (all platforms, no Docker)
make ci-coop

# Windows cross-compilation (requires Docker or mingw-w64)
make docker-ci-windows       # Docker (recommended)
make ci-windows              # or: brew install mingw-w64 (macOS) / apt install mingw-w64 (Linux)

# WebAssembly (requires Docker)
make docker-ci-wasm

# ARM embedded (requires Docker)
make docker-ci-embedded

# All portability checks at once
make ci-portability
```

**Platform coverage:**

| Your platform | `make ci` covers | Cross-platform via Docker |
|--------------|-----------------|--------------------------|
| macOS | macOS Clang | `docker-ci-windows`, `docker-ci-wasm`, `docker-ci-embedded` |
| Linux | Linux GCC | `docker-ci-windows`, `docker-ci-wasm`, `docker-ci-embedded` |
| Windows (MSYS2) | Windows MinGW | `docker-ci-wasm`, `docker-ci-embedded` (Docker on WSL2) |

**No OS can locally test another OS natively.** macOS cannot be virtualized on Linux/Windows. Windows build+run requires MSYS2. Docker targets provide cross-compilation syntax checking only. If your changes touch platform-specific code (`_WIN32`, `__APPLE__`, `system()`, file paths, symlinks, PATH lookup, process spawning, sockets), wait for CI results before merging.

### CI permutations run on every PR

GitHub Actions runs a matrix of builds on every pull request. Every target
must be green before a PR can be merged. Plan your code changes with this
matrix in mind:

| Target | Runner | Compiler | What trips up PRs |
|---|---|---|---|
| **Linux GCC** | `ubuntu-latest` | `gcc` | `-Werror` strictness; pedantic `-Wall -Wextra` |
| **Linux Clang** | `ubuntu-latest` | `clang` | Different warning surface than GCC |
| **macOS ARM64** | `macos-latest` | Apple Clang | BSD-style tools; no `/proc`; different `stat(2)` fields; Gatekeeper on fresh binaries |
| **macOS x86_64** | `macos-13` | Apple Clang | Same as ARM64 plus intel-specific codegen corners |
| **Windows MSYS2** | `windows-latest` (MSYS2 shell) | MinGW GCC | `#ifdef _WIN32` branches actually execute; POSIX syscalls (`symlink`, `readlink`, `fork`, `execvp`, `pipe`) are absent or stubbed; `/bin/sh` / `rm` / `ln` not guaranteed; path separators; `_mkdir`/`_unlink` instead of `mkdir`/`unlink`; `$PATH` uses `;` not `:` |
| **Windows mingw-w64** | `windows-latest` (cross-compiled) | `x86_64-w64-mingw32-gcc` | Same C-level issues as MSYS2 but a different toolchain version, so MSVCRT corner-cases sometimes diverge |
| **`make ci-coop`** | any Linux | `gcc` with `AETHER_NO_THREADING` | Cooperative scheduler only — tests that assume pthreads / `spawn` semantics may behave differently |
| **`make test-asan`** | Linux | AddressSanitizer | Any use-after-free / leak in your new C code |
| **`make test-valgrind`** | Docker Linux | Valgrind | Same, slower, catches a slightly different set |

The Windows targets are where most new-feature PRs fail first, because
the existing stdlib has `_WIN32` stubs for anything POSIX-specific
(symlinks, `fork`/`exec`, `readlink`, `/proc`, signals, dotfiles-by-default,
`:` as PATH separator). If your feature adds a new stdlib function, you
should assume Windows gets a stub that returns failure until a proper
Win32 backend lands, and your tests should **detect the stub and skip
gracefully** rather than assert success and crash the matrix.

### Coding for portability

Anticipate the CI permutations while writing the feature, not after the
first red build. A few patterns the stdlib and existing tests use:

**1. Detect the platform at the top of a test and skip the parts that
don't apply.** Aether programs can read `$OS` — it's set to `Windows_NT`
on both MSYS2 and mingw-w64 (cmd.exe inherits it, bash picks it up). This
is cleaner than probing a stub function because the SKIP message
self-documents *why*:

```aether
import std.os
main() {
    if os_getenv("OS") == "Windows_NT" {
        println("SKIP os_which: Windows backend not yet implemented")
        return
    }
    // …POSIX-only assertions below…
}
```

**2. Alternatively, probe a call that's known to fail on the platforms
you haven't implemented.** Existing example:
`tests/syntax/test_await_io.ae` calls `net.pipe_open()` and prints
`SKIP: pipe() unavailable (non-POSIX platform)` if the return is
negative. This is useful when the platform boundary is narrower than
"all of Windows" — e.g. a stub that happens to fail, or a feature that
depends on a capability you can't name directly.

**3. Split a test that has both portable and non-portable sub-cases.**
In `tests/syntax/test_fs_stdlib_bundle.ae`, `fs_mkdir_p` and `fs_unlink`
work on Windows (they wrap `_mkdir` / `_unlink`), but `fs_symlink`,
`fs_readlink`, and `fs_is_symlink` are stubbed. The test runs mkdir_p
and unlink on every platform, and wraps the symlink sub-cases in
`if is_windows == 0 { … }`. Each platform gets the coverage it can
actually provide.

**4. On the C side, stub don't fake.** When you add a POSIX function
to a stdlib `.c` file, wrap the real implementation in `#ifndef _WIN32`
and provide a Windows branch that returns 0 / NULL / -1 following
whatever convention the rest of the file uses. Do NOT try to emulate
the POSIX behavior using the closest Win32 API unless you've actually
tested it — the CI will catch it, but so will your users, and a stub
that fails loudly is better than a half-broken fake. File a follow-up
issue for the real Windows backend at the same time so it doesn't
rot.

**5. On the C side, use `<errno.h>` after every POSIX syscall, not
`system()` of an equivalent shell command.** `system("ln -s …")` doesn't
exist on Windows (no `/bin/sh`, no `ln`). Direct `symlink(2)` at least
has a clean `#ifndef _WIN32` boundary.

**6. Prefer existing portable helpers over re-rolling your own path
handling.** `std/fs/aether_fs.c` provides `path_join`, `path_dirname`,
`path_basename`, `path_normalize`, `path_is_absolute`. These already
handle the cases where `/` and `\` both count as separators (Windows
C stdlib accepts either). Concatenating with `"/"` is portable by
accident; using the helpers is portable by design.

**7. When in doubt, run the tests under a forced `OS=Windows_NT` env
var locally.** It won't exercise actual `_WIN32` C branches (you need
mingw-w64 or Docker for that), but it will exercise the Aether-level
skip guards you wrote, which is where most per-test PR breakage lives:

```bash
OS=Windows_NT build/test_syntax_test_my_new_test
```

### Additional Checks

1. **No memory leaks**
   ```bash
   make docker-ci              # Includes Valgrind + ASan in Docker
   # Or locally if valgrind is installed:
   valgrind ./build/test_runner
   ```

2. **Add tests for new features**
   - New feature = new test
   - Bug fix = regression test

3. **Update documentation**
   - Add/update comments in code
   - Update README.md if adding user-facing features
   - Update docs/ if changing language behavior
   - Update CHANGELOG.md under `[current]`

### PR Template

```markdown
## Description
Brief description of changes

## Type of Change
- [ ] Bug fix
- [ ] New feature
- [ ] Performance improvement
- [ ] Documentation update

## Testing
- [ ] Added tests for new functionality
- [ ] All tests pass (make test)
- [ ] Valgrind reports no leaks
- [ ] Tested on Linux/macOS/Windows (if applicable)

## Performance Impact
- [ ] No performance impact
- [ ] Performance improvement (include benchmarks)
- [ ] Performance regression (explain why acceptable)

## Checklist
- [ ] Code follows style guidelines
- [ ] Self-review completed
- [ ] Comments added for complex logic
- [ ] Documentation updated
```

### Review Process

1. Automated CI checks must pass (Linux/macOS, memory safety, benchmarks)
2. Code review by maintainer
3. Address feedback
4. Merge when approved

## Code review heuristics

The patterns below come up often in review. Each example contrasts a
weak form (BAD), an acceptable form (GOOD), and where applicable a
stronger form (BETTER). Use them as a checklist before sending a PR.

### Memory Management

```c
// BAD: Memory leak
char* create_string(void) {
    char* str = malloc(100);
    strcpy(str, "test");
    return str;  // Caller must remember to free
}

// GOOD: Document ownership
// Returns: Newly allocated string (caller must free)
char* create_string(void) {
    char* str = malloc(100);
    strcpy(str, "test");
    return str;
}

// BETTER: Use arena allocation
char* create_string(Arena* arena) {
    char* str = arena_alloc(arena, 100);
    strcpy(str, "test");
    return str;  // Freed when arena is freed
}
```

### Error Handling

```c
// BAD: Ignoring errors
FILE* f = fopen("file.txt", "r");
fread(buffer, 1, 100, f);  // Crashes if f is NULL

// GOOD: Check for errors
FILE* f = fopen("file.txt", "r");
if (!f) {
    fprintf(stderr, "Failed to open file\n");
    return -1;
}
defer(fclose(f));
```

### Platform-Specific Code

```c
// BAD: Linux-specific
#include <unistd.h>
usleep(1000);

// GOOD: Cross-platform
#ifdef _WIN32
    #include <windows.h>
    Sleep(1);  // milliseconds
#else
    #include <unistd.h>
    usleep(1000);  // microseconds
#endif
```

## Versioning and Release Process

Aether uses [Semantic Versioning](https://semver.org/). Releases are fully automated.

### How it works

1. **Source of truth**: Git tags (`v*.*.*`). The `VERSION` file is kept in sync for non-git contexts (release tarballs, binary installs).

2. **Automatic release**: Every merge to `main` triggers `.github/workflows/release.yml`, which:
   - Computes the next version from the highest existing `v*.*.*` tag
   - Updates the `VERSION` file on `main` and commits `chore: release X.Y.Z`
   - Tags the commit and pushes both to `main` and the tag
   - Builds binaries for Linux, macOS (arm64 + x86_64), and Windows
   - Creates a GitHub Release with all artifacts

3. **Version bump rules**:
   - Commit message starts with `major` → bumps MAJOR (e.g., 0.17.0 → 1.0.0)
   - Anything else → bumps MINOR (e.g., 0.17.0 → 0.18.0)

4. **Race prevention**: The workflow uses a `concurrency` group — if two PRs merge in quick succession, the second release queues until the first completes.

### Where the version appears

| Component | How it gets the version |
|-----------|------------------------|
| `make ae` / `make compiler` | Makefile reads `git tag -l`, falls back to `VERSION` file |
| `ae version` | Compiled-in via `-DAETHER_VERSION` from Makefile |
| `aetherc --version` | Compiled-in via `-DAETHER_VERSION` from Makefile |
| `install.sh` | Reads `VERSION` file |
| Release tarballs | `VERSION` file baked into the archive |
| Windows native builds | Reads `VERSION` file (no git dependency) |

### For contributors

- **Never edit the `VERSION` file manually** — it's updated automatically by the release workflow
- **Never create `v*.*.*` tags manually** — let the workflow handle it
- **Always update `CHANGELOG.md`** when adding features or fixes (see below)

### Changelog convention: `[current]`

All new changes go under the `## [current]` section at the top of `CHANGELOG.md`. **Do not invent a version number** — just add your entry under `[current]`.

When your PR merges to `main`, the release pipeline automatically:
1. Computes the next version from the highest existing git tag
2. Replaces `## [current]` with `## [X.Y.Z]` (the new version number)
3. Commits the updated `CHANGELOG.md` and `VERSION` file
4. Tags, builds, and publishes the release

**Example workflow:**

```markdown
## [current]

### Added
- My new feature description

### Fixed
- Bug fix description
```

After merge, the pipeline transforms this into:

```markdown
## [0.22.0]

### Added
- My new feature description

### Fixed
- Bug fix description
```

**Rules:**
- Use [Keep a Changelog](https://keepachangelog.com/) categories: `Added`, `Fixed`, `Changed`, `Removed`, `Deprecated`
- One `[current]` section at a time — if it already exists, add your entries to it
- If `[current]` is missing, create it at the top (below the header)
- Keep entries concise but specific — mention what changed and why

### `### Upgrade notes` for memory-fix releases

When a PR touches `compiler/codegen/codegen_stmt.c` (the heap-string-tracker wrapper), `runtime/aether_string.c` (the refcount allocator), or anything else that changes when the runtime frees a heap-allocated value, add an `### Upgrade notes` block to the same `[current]` entry. The block names the alias / ownership patterns whose downstream behaviour the change can break, and gives a recommended pre-upgrade play.

The hazard the section guards against: a memory-fix release that closes a leak can flip latent UAFs in downstream code from "harmless slow leak" to "hard crash". The leak was kindly keeping the dangling pointer alive; once the leak is fixed, the alias dangles for real. If the CHANGELOG doesn't call this out, downstreams hit "rebuilt under new aetherc, my server crashes" and run a manual debugging ladder to figure out what changed semantically.

**Template:**

```markdown
### Upgrade notes

This release tightens <ownership behaviour>: <one-sentence statement of the new
runtime guarantee>. The previous version <one-sentence statement of what was
loose / broken before>.

If your project <names the aliasing pattern that's now dangerous — e.g. stores
a heap-string in a long-lived structure and also keeps a local pointer that's
later reassigned>, the previous compiler tolerated this as <a leak / a no-op>;
this release will <name the new visible behaviour — free the alias / panic
on the use / etc.>. This shape is a use-after-free / double-free / etc.

**Recommended pre-upgrade play:**

1. <First step — usually `aetherc --diagnose=ownership` or a similar audit>.
2. <Second step — what to look for in the audit output>.
3. <Third step — sanitiser run, integration test, etc.>
```

Only include the section when the change can plausibly break downstream code that compiled fine before. A pure leak fix that has no UAF surface (e.g. the wrapper is right and only the diagnostic is new) doesn't need one. When in doubt, write the section — the cost of an unneeded note is low; the cost of a surprised porter is a debugging day.

### Building locally

```bash
make ae          # Picks up version from git tags automatically
./build/ae version   # Verify: should show the latest tag
```

If you're building outside a git repo (e.g., from a release tarball):

```bash
make ae          # Falls back to VERSION file
```

## Getting Help

- GitHub Issues: Report bugs and request features
- Discussions: Ask questions and share ideas
- Code Comments: Explain complex implementations
- Documentation: Check docs/ folder for language details

## License

By contributing to Aether, you agree that your contributions will be licensed under the MIT License.
