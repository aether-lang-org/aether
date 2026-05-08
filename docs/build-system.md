# Build System

## Overview

Aether uses a multi-tier build system with different optimization profiles for development, testing, and release builds.

## Project Configuration (`aether.toml`)

Every Aether project has an `aether.toml` at its root. `ae run` and `ae build` read it automatically.

### Minimal project

```toml
[package]
name = "myapp"
version = "0.1.0"

[[bin]]
name = "myapp"
path = "src/main.ae"
```

### Full configuration reference

```toml
[package]
name = "myapp"
version = "1.0.0"
description = "What this program does"

[build]
# Extra C compiler flags applied during `ae build` (release builds only).
# `ae run` uses -O0 for fast iteration regardless of this setting.
cflags = "-O3 -march=native"

# Platform-specific linker flags (e.g. for third-party C libraries).
# macOS/Linux: link_flags = "-lraylib"
# Windows:     link_flags = "-Ldeps/raylib/lib -lraylib -lopengl32 -lgdi32 -lwinmm"
link_flags = ""

[[bin]]
name = "myapp"
path = "src/main.ae"

# Extra C source files compiled alongside the Aether output.
# Useful for C FFI helpers, renderer backends, or any C code your program needs.
# Merged additively with any --extra flags passed on the command line.
extra_sources = ["src/ffi_helpers.c", "src/renderer.c"]
```

### `extra_sources` vs `--extra`

Both add C files to the build — they are additive when both are present.

| | `extra_sources` in `aether.toml` | `--extra file.c` CLI flag |
|---|---|---|
| **Scope** | Always included for this binary | Per-invocation |
| **Good for** | C helpers your program always needs | Renderer backends, platform variants |
| **Works with** | `ae build` and `ae run` | `ae build` and `ae run` |

### `--quick` for fast iteration

By default, `ae build` runs the C compiler with `-O2` to match release-quality codegen. For tight edit/build/test loops where binary speed isn't critical, pass `--quick` to drop to `-O0 -g`:

```sh
ae build src/main.ae           # release-shape, -O2 (default)
ae build src/main.ae --quick   # iteration-shape, -O0 -g
```

`--quick` typically halves the gcc step on small programs, at the cost of unoptimised codegen. `ae run` already uses `-O0` regardless, since cache hits dominate over a single optimised compile.

### Resolving the build target

`ae build` accepts either a path to a `.ae` file or a `[[bin]]` name from `aether.toml`. The two are equivalent:

```sh
ae build src/main.ae   # explicit path
ae build myapp         # [[bin]] name = "myapp"
```

When the positional argument doesn't exist as a file, `ae` checks `aether.toml`'s `[[bin]]` entries for a matching `name = "..."` and uses that bin's `path` field. Cargo and similar build systems work the same way.

If you run `ae build` from a subdirectory and there's no `aether.toml` in the current directory, `ae` walks up the directory tree looking for one. When it finds an ancestor `aether.toml`, it switches to that directory before resolving paths — so `cd src && ae build main.ae` works as if you had run `ae build src/main.ae` from the project root, and `extra_sources` declared in the toml are still applied. Walk-up only happens when there's no toml in the current directory; a project with a local `aether.toml` always wins.

---

## Build Cache

Both `ae run` and `ae build` cache compiled binaries in `~/.aether/cache/`. The cache key is an FNV64 hash of:

- The source file's content
- The `aetherc` binary's mtime (recompile invalidates everything)
- `libaether.a`'s mtime (stdlib rebuild invalidates everything)
- Every `--extra` C file's *content* (editing an FFI shim invalidates the cache, not just touching it)
- The optimisation level (`-O0` for `ae run` and `ae build --quick`, `-O2` for default `ae build`)

`ae run` and `ae build` use separate cache slots so toggling between them doesn't churn one entry back and forth.

**Cost breakdown of a build:**
- Cache hit (`ae build`): copy the cached binary to the requested output path. Sub-millisecond on local disk.
- Cache hit (`ae run`): fork + exec the cached binary directly.
- Cache miss: full `aetherc` front-end + gcc compile + link. Dominant cost is gcc; `aetherc` is a small fraction.
- First macOS run: an extra one-time pause while the OS performs its Gatekeeper check on the newly compiled binary. Subsequent runs of the same cached binary are hit-path.

```bash
ae cache          # Show cache location and entry count
ae cache clear    # Delete all cached builds
```

Wasm builds, `--emit=lib`, and `--namespace` SDK generation skip the cache (different artefact shapes; each will get its own cache layout when measurement justifies it).

---

## Build Tiers

All builds go through the Makefile, which handles the full source list across subdirectories (`compiler/parser/`, `compiler/analysis/`, `compiler/codegen/`, `runtime/scheduler/`, `runtime/memory/`, etc.).

### Development Build

```bash
make compiler CFLAGS="-O0 -g -Icompiler -Iruntime -Iruntime/actors -Iruntime/scheduler -Wall -Wextra -Wno-unused-parameter"
```

**Purpose**: Fast compilation with debug symbols for active development.

### Testing Build

```bash
make compiler    # Uses -O2 by default
```

**Purpose**: Moderate optimization for CI and testing.

### Release Build

```bash
make compiler CFLAGS="-O3 -march=native -flto -Icompiler -Iruntime -Iruntime/actors -Iruntime/scheduler -Wall -Wextra -Wno-unused-parameter"
```

**Purpose**: Full optimization for production use and benchmarking.

**Flags:**
- `-O3`: Aggressive inlining, loop unrolling, auto-vectorization
- `-march=native`: CPU-specific instruction selection
- `-flto`: Link-time optimization for cross-translation-unit inlining

### Profile-Guided Optimization

```bash
# Stage 1: Instrument
gcc -O3 -march=native -fprofile-generate -o aetherc_pgo ...

# Stage 2: Profile (run representative workload)
./aetherc_pgo <typical_usage>

# Stage 3: Optimize with profile data
gcc -O3 -march=native -fprofile-use -o aetherc ...
```

PGO uses runtime profiling data to improve branch prediction, function inlining decisions, and code layout. It is used by major projects including Chrome, Firefox, CPython, and LLVM for their release builds.

## Incremental Compilation

**Dependency Tracking:**
```makefile
CFLAGS += -MMD -MP
-include $(DEPS)

build/%.o: %.c
    $(CC) $(CFLAGS) -c $< -o $@
```

The `-MMD` flag generates `.d` dependency files listing all headers included by each source file. Make uses these to rebuild only modified files and their dependents.

## Parallel Compilation

```bash
make -j8    # 8 parallel jobs
```

Limited by dependency ordering: some files must build before others.

## Cross-Compilation (PLATFORM variable)

The `PLATFORM` Makefile variable selects the scheduler backend and sets platform-specific flags:

```bash
# Native (default) — multi-core scheduler, pthreads
make stdlib PLATFORM=native

# WebAssembly — cooperative scheduler, no pthreads/fs/net
make stdlib PLATFORM=wasm    # CC=emcc, -DAETHER_NO_THREADING/FILESYSTEM/NETWORKING

# Or use the ae CLI directly:
ae build --target wasm hello.ae    # Produces hello.js + hello.wasm
node hello.js                       # Run with Node.js

# Embedded — cooperative scheduler, no pthreads/fs/net/getenv
make stdlib PLATFORM=embedded    # -DAETHER_NO_THREADING/FILESYSTEM/NETWORKING/GETENV

# Override individual features on native
make stdlib EXTRA_CFLAGS="-DAETHER_NO_THREADING"    # Auto-selects cooperative scheduler
make stdlib EXTRA_CFLAGS="-DAETHER_NO_FILESYSTEM -DAETHER_NO_NETWORKING"
```

The Makefile auto-detects `AETHER_NO_THREADING` in `EXTRA_CFLAGS` and switches to the cooperative scheduler automatically. It also omits `-pthread` from linker flags.

### Docker-Based Cross-Compilation

For cross-compilation without installing toolchains locally:

```bash
make docker-ci-wasm        # Emscripten SDK → compile + run with Node.js
make docker-ci-embedded    # arm-none-eabi-gcc → syntax-check
make ci-portability        # All: native coop + WASM + embedded
```

Docker images: `docker/Dockerfile.wasm` (Emscripten), `docker/Dockerfile.embedded` (ARM Cortex-M4).

## Build Recommendations

| Use Case | Flags | Notes |
|----------|-------|-------|
| Development | `-O0 -g` | Fast iteration, debug symbols |
| Testing/CI | `-O2` | Balanced optimization |
| Release | `-O3 -march=native -flto` | Full optimization |
| Profiling | PGO pipeline | Based on representative workload |
| Hardened | `HARDEN=1` | See "Hardening" section below |
| WASM | `PLATFORM=wasm` | Cooperative scheduler, Emscripten |
| Embedded | `PLATFORM=embedded` | Cooperative scheduler, no OS |

## Hardening (`HARDEN=1`)

Opt-in hardening flags add stack canaries, fortified libc-call wrappers, and format-string-injection diagnostics. Enabled with the `HARDEN=1` environment variable; disabled by default in release builds because the runtime overhead is non-zero (~3-5% on micro-benchmarks) and macOS Clang has historically been finicky with `_FORTIFY_SOURCE` on a few setups.

```bash
# Local hardened build — recommended before submitting a PR that
# touches C in compiler/, runtime/, or std/.
HARDEN=1 make compiler ae stdlib

# Hardened CI: run the full suite end-to-end under hardening.
HARDEN=1 make ci
```

Flags enabled (issue #396):

| Flag | Purpose |
|------|---------|
| `-fstack-protector-all` | Stack canaries on every function (not just gcc-strong heuristic candidates) — catches the smashing class of bugs that escape the default heuristic. |
| `-D_FORTIFY_SOURCE=2` | Runtime checks on `read`/`write`/`memcpy`/`strncpy`/`printf`-family calls. Linux `gcc` also emits compile-time warnings when it can prove a buffer overflow — those should be fixed at the source, never blanket-suppressed. Requires at least `-O1`; the default `-O2` satisfies that. |
| `-Wformat -Wformat-security` | Diagnose `printf`-family format strings sourced from non-literals (the `%s`-format-injection class). Default in modern Linux distros; we standardise on it explicitly. |

The flags are added to `CFLAGS` only when `HARDEN=1` is set; the default build path is byte-identical to the unhardened release. The Linux/Hardened (gcc) CI matrix entry pins this so a regression that introduces an unchecked `memcpy`-over-fixed-buffer trips a red check before merge.

## References

- GCC Optimization Options: https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html
- LLVM PGO Guide: https://llvm.org/docs/HowToBuildWithPGO.html
