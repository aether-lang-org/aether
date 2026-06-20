# Notes to self (LLM assisting on Aether)

Not a CLAUDE.md — short, opinionated, written for a future LLM picking up
mid-task. Re-read at start of every session.

## What Aether is, in one paragraph

Systems language. Compiles to C. Two emit modes: `--emit=exe` (default,
produces a binary with a `main()`) and `--emit=lib` (produces `.so`/`.dylib`
with ABI-stable `aether_<name>` exports for FFI from C / Python / Ruby / Java
via ctypes/SWIG/Panama). `--emit=lib` is capability-empty by default —
`std.fs` / `std.net` / `std.os` imports are rejected. The opt-in is
`--with=fs[,net,os]`, an explicit per-build flag for projects that ARE
the host and want full syscall access (e.g. implementing a systems tool
in Aether + a thin C driver).

## How to anchor Aether against languages you already know

Think of it as: **Go's ergonomics + Rust's capability discipline +
Erlang's actor syntax, compiling via C.**

- **NOT Rust** — no borrow checker, no ownership, no lifetimes. Strings
  are ref-counted or arena-owned; you release explicitly where it
  matters, drop-on-scope-exit elsewhere.
- **NOT Go** — no GC. Memory is manual at the runtime C level; Aether
  code mostly avoids the issue via RAII-via-codegen.
- **NOT C** — has closures, tuples, Go-style `(value, err)` returns,
  string interpolation, pattern matching.
- **Actor model is Erlang-ish** but message types are declared, not
  duck-typed. `receive` + `send`, not `!` / mailbox-matching.
- **Has some sandboxing features build in** - Three
  layers: `--emit=lib` + `--with=` gates stdlib imports at compile
  time; `hide` / `seal except` denies enclosing names per lexical
  block; `libaether_sandbox.so` (LD_PRELOAD) checks libc calls
  against a builder-DSL grant list. If you like: a mashup of Pony 
  object capabilities, Java's removed SecurityManager, and a fraction 
  of gVisor. 
- **Hosts other languages** - Granted thss is only slightly better 
  than a linked lib, 
  Aether `main()` embeds Lua/Python/Perl/Ruby/Tcl/JS in-process via
  `contrib.host.<lang>.run_sandboxed(perms, code)`;
  Java/Go/aether-hosts-aether are separate-process. Same grant list
  + LD_PRELOAD gates the hosted interpreter's libc calls too. Guest
  direction: `--emit=lib` → Python ctypes / Java Panama / Ruby
  Fiddle SDKs auto-generated from `aether_describe()`.
- **NOT quite Ruby/Smalltalk/Groovy's builder-style closures**
  — those all run on a VM (MRI, the Smalltalk image, the JVM)
  where the block has runtime reflection access to its surrounding
  scope, regardless of whether it was reached by interpretation
  (Ruby, classic Smalltalk) or bytecode compilation (Groovy since
  ~2012, modern Ruby JIT). Aether is native: closures lower to plain
  C functions with no VM behind them, so there's no run-time hook
  for the block to reach back through. `hide` / `seal except` are
  compile-time, and the grant list is the closure's only handle to
  privileged operations. The trailing-block + `builder` shape is
  Aether's lever for **pseudo-declarative nirvana**: the call site
  reads like a config file (bare setters, no constructor noise, no
  parameter threading) while the body is full Aether underneath —
  control flow, env lookups, library calls, the lot. Levels 1-2
  (YAML, HCL) are readable but powerless; Level 3 (Pulumi/CDK) is
  powerful but reads like glue code; Aether's closure-DSL is the
  one step beyond that gives both. This is what makes the next
  bullet possible.
- **NOT a YAML/HCL/Helm host** (related) — and won't be. Building on
  the closure-DSL above, Aether's pitch for server-shaped libraries
  is **config IS code**: don't ship a YAML loader, expose your
  start-surface as a closure-DSL block and let the operator's
  "config" be a `.ae` file they run with `ae run`. Same file is
  config + validation + conditionals + entry point; no second
  parser, no template language, no embedded scripting hack.
  Sandboxing (`--emit=lib --with=...`, `hide`, `seal except`)
  keeps untrusted configs safe — the same machinery that makes the
  embedded-DSL case work. If a downstream user asks "should we add
  a YAML config loader?", point them at `docs/config-is-code.md`
  and the trailing-block DSL pattern.

Sandbox more info: Full comparison (who brings what — Pony's per-reference
  granularity, Java SecManager's policy-file ancestry, gVisor's
  syscall scope, plus WASI/Deno/Rust) lives in
  `docs/containment-sandbox.md` → *How Aether compares to other
  capability / sandbox systems*.

Syntax looks Go-ish at a glance (braces, `func_name(x: int) -> int`).
Don't overshoot — there's no `go` keyword, no channels (send-to-actor
plays that role), no interfaces.

## Files/dirs worth knowing

- `CHANGELOG.md` — reverse-chron, `[current]` section holds unreleased
  work. Everything since the last tagged release sits there. Read the
  top 40 lines at session start to know what just landed.
- `docs/emit-lib.md` — the capability-opt-in doc. Canonical reference
  for why `std.fs` is banned under `--emit=lib` by default.
- `docs/config-is-code.md` — the "don't ship a YAML loader" pitch.
  Library authors with a "start the thing" surface should expose
  it as a trailing-block DSL, not a config-file format. Pair with
  `docs/closures-and-builder-dsl.md` for the mechanism. `docs/cic-help.md`
  is the (proposed, unimplemented) `ae help <script>` companion that
  catches operator-side mistakes in those scripts.
- `docs/next-steps.md` — roadmap. P1/P2/P3/P4 are ordered; `fs.copy` /
  `fs.move` / `fs.chmod` / `fs.symlink` / `fs.realpath` are P4. Check
  here before speccing a new stdlib addition.
- `std/<module>/module.ae` — the Aether-facing surface. Raw externs
  end in `_raw`; Go-style wrappers return `(value, err)` tuples.
- `std/<module>/aether_<module>.c` — the C runtime behind the externs.
- `contrib/<module>/` — opt-in modules NOT in `libaether.a`. Same
  shape as `std/*` (module.ae + aether_*.c + README), but consumers
  add `extra_sources` + `link_flags` per the contrib README rather
  than getting auto-link. **Always check `contrib/` before deciding
  a stdlib gap is real** — a porter writing a downstream port may
  not realise the module is there. Today covers: `xml/expat` (SAX
  XML parser via libexpat), `sqlite`, `host/{python,ruby,lua,perl,
  tcl,duktape,tinygo,go,java}` for embedded interpreters,
  `climate_http_tests`, `tinyweb`.
- `compiler/aetherc.c` — CLI entry. `--emit=lib`, `--with=`, import
  gate lives around line 590.
- `build/aetherc`, `build/ae` — the compiled binary. `make && make
  install` to rebuild. The binary is SHA-pinned per commit; if the
  CHANGELOG `[current]` mentions features not in `aetherc --help`,
  the binary is stale and needs rebuilding.
- `patch_json_plan.md`, `stdlib_wish.md` — spec documents written by
  downstream users (svn-aether port) requesting changes. Good model
  for incoming feature requests: exact API shape, call-site census,
  rationale. When a wish lands, the wish file gets a status banner
  at the top but stays in-tree as context.
- `tests/regression/` — one `.ae` file per feature; the CI gate.
  `tests/integration/` — integration test directories (e.g.
  `emit_lib_with_capability/` for the `--with=` opt-in).

## Idioms that keep biting

- **Go-style returns, not tuples-as-values.** `fs.write_atomic(path,
  data, len) -> string` — empty string = success, non-empty = error
  message. Don't "improve" the convention, it's consistent across all
  of `std`.
- **Split-accessor pattern for multi-return.** Where a wrapper needs
  more than one return value and the language can't yet unify tuples
  cleanly across FFI, the pattern is a try/get pair backed by TLS:
  `fs_try_read_binary(path) -> int` writes into a TLS buffer,
  `fs_get_read_binary()` / `fs_get_read_binary_length()` / `fs_release_read_binary()`
  drain it. Mirrors the paths_index / checksum pattern elsewhere.
- **Strings are length-aware internally**, but `string_concat("", raw)`
  treats `raw` as C-string (strlen-bounded) and will truncate at the
  first embedded NUL. Binary-safe reads must go through the raw
  `fs_get_read_binary()` + `_length()` pair and memcpy into caller
  storage, not through the `fs.read_binary` wrapper.
- **Reserved keywords that trip users up**: `state`, `match`,
  `message` (actor-model hangover). Fails in extern param names too.
  Rename to `st`, `is_match`, `msg`. `after` parses as something
  scanner-special too — rename locals to `b_after` / `tail_after`
  if you hit a "Expected statement in block" at an `if x = call(),
  …` use site.
- **Runtime sequence of strings → `*StringSeq`, not `string[]`.**
  `string[]` lowers to bare `const char**` (no length, no refcount).
  `*StringSeq` (`std.string` surface — `string.seq_cons`,
  `string.seq_head`, `string.seq_tail`, `string.seq_length`,
  `string.seq_free`, etc.) is O(1) head/tail/cons/length, refcount-
  aware, structurally shared, pattern-matches with `[h|t]`. Same
  `[a, b, c]` literal builds a cons chain when target is
  `*StringSeq` (message field) or a static C array when target is
  `string[]`. `string.split_to_seq` is the runtime entry point.
- **Unqualified imports are spelt `import mod (*)`, not `import mod unqualified`.** Aether ships glob imports — `import std.math (*)` brings every public symbol into the bare namespace, no `math.` prefix. Selective form `import std.math (sqrt, pow)` is the same shape with an enumeration. There is no `unqualified` keyword and no `use mod::*` (Rust) or `from mod import *` (Python) spelling — just the parenthesised `(*)`. Applies equally to stdlib, contrib, and local modules. When a porter or doc asks for "Java-style import static" or "Rust use", point at this. (Language reference: "Glob Import" section.)
- **`<<MARKER … MARKER` heredocs.** Literal string, no interpolation/escaping.
  Reach for them to embed another language's source verbatim
  (`contrib.host.*` snippets, SQL) — no `\"` on the guest's own quotes. Use
  `"…"` when you need `${}`. **Common-indent dedent (default):** the longest
  leading-whitespace prefix shared by every non-blank line is stripped, so you
  can indent the body to match surrounding code without that indent leaking
  into the string. Blank lines don't constrain the prefix; relative indentation
  within the block is kept. The match is character-exact — a space-vs-tab
  mismatch at a column stops the strip there (no shifting past a disagreement),
  so to keep a literal common indent, indent one line less than the rest. The
  closing marker must be at column 0. (lexer.c `<<` case.)
- **Trailing closure brace must be on the call's line.** `f(x) { … }`
  attaches as a trailing closure; `f(x)\n{ … }` is parsed as a
  separate bare-brace block. The compiler warns on the next-line
  variant. Move the brace to the call's line if you wanted a
  closure. Pre-fix, the next-line shape was eaten as a trailing
  closure even when the user meant a separate block, producing
  misleading "Undefined variable" errors against names assigned
  by the call. (#286)
- **Ownership of `ptr`-typed returns.** Strings returned by builtins
  like `string.from_long` / `string.concat` are ref-counted
  (`AetherString*` with a magic sentinel). Safe to pass to other
  string ops without explicit release; they get dropped when the
  last ref goes. Strings returned by `extern fs_foo_raw() -> string`
  are borrowed from a TLS buffer or arena — valid only until the
  next same-kind call or explicit release. If it smells like `ptr`,
  assume borrowed; if it smells like `string` from a builtin, assume
  ref-counted.
- **`extern f() -> ptr` type-erases length.** Aether strings are
  length-aware internally, but once they cross an extern boundary
  as `ptr`, Aether sees only the leading bytes up to the first
  NUL. This is why `fs.read_binary` has a paired `_length()`
  accessor, not a single-return.
- **Spawning a child binary uses `std.os`, not a separate process
  module.** `os.run_capture(prog, argv, env) -> (stdout: string,
  exit_code: int, spawn_err: string)` is the canonical "fork + exec
  + wait + capture-stdout" — argv-based, no shell, binary-safe.
  Caveat: only stdout is captured today; child stderr passes
  through to the parent process. The third return slot is the
  spawn-error string ("" on successful spawn even if exit_code != 0;
  non-empty only on fork/exec/sandbox failure). Sibling externs:
  `os_run` (no capture, just exit code), `os_system` (legacy
  `system(3)` shell-out, prefer `run_capture`), `os_execv` (replace
  current process). Don't propose a new `std.process`.
- **XML parsing lives in `contrib`, not `std`.** `contrib.parsers.xml_expat`
  is a SAX-style libexpat wrapper — start/end/text callbacks,
  attribute access, entity decoding, multi-chunk streaming. Add
  `extra_sources = ["contrib/parsers/xml_expat/aether_xml_expat.c"]` and
  `link_flags = "-lexpat"` per the contrib README. Don't hand-roll
  an `index_of`-based parser — that exact mistake landed in
  fbs-core's S3 port (#627) because the porter didn't realise
  `contrib.parsers.xml_expat` existed.
- **Operator-supplied Liquid templates → `contrib.templating.liquid`.**
  Pure-Aether Shopify-Liquid port. Use it for email bodies, dashboards,
  generated reports — anywhere a trusted-human operator writes a
  template but you don't want their template to execute arbitrary
  code. `{% include %}` / `{% render %}` need an explicit
  `context_set_include_root(ctx, root)` (path traversal blocked via
  `fs.is_within_base`). Phase 1 ships scalar typed bindings
  (`context_put_int`/`_float`/`_bool`/`_nil`); array / object / dotted
  path access is Phase 2. Don't hand-roll a Mustache or own-syntax
  templater unless you have a specific reason — this is the supported
  path.
- **HTTP client lives in two tiers, server in one.** `import
  std.http` gives v1 client one-liners (`http.get(url) -> (body,
  err)`, `http.post`, `http.put`, `http.delete`) plus the entire
  server surface (`http.server_create(port)`, route handlers, etc).
  `import std.http.client` gives a v2 builder (`client.request(method,
  url)` → `set_header` → `set_timeout` → `send_request`) when the
  one-liners are insufficient — anything needing custom request
  headers, custom HTTP methods, response status discrimination,
  per-request timeouts, or response headers. Reach for v2 by default
  for non-trivial client work; v1 is fine for "GET, expect 200, want
  body". Server-side stays in `std.http` (there's no v2 server). For
  record/replay HTTP testing, the Servirtium VCR engine that used to be
  `std.http.server.vcr` now lives in its own repo,
  [`servirtium-vcr`](https://github.com/aether-lang-org/servirtium-vcr)
  (`import core.vcr`) — it is no longer part of the Aether stdlib.

## Working with downstream users

- **svn-aether port (avn)** (`~/scm/subversion/subversion/` locally;
  GitHub: anthropic/avn — sibling claude works there) is the biggest
  real-world consumer. Port is methodical C → Aether, one-leaf-per-
  commit. Downstream finds the gaps before anyone else — every
  cross-`import` typer issue (struct visibility, selective-import
  propagation, 128-decl cap) was filed by avn before showing up
  anywhere else.
- **aether-ui** (`https://github.com/aether-lang-org/aether-ui`) —
  cross-platform widget toolkit (GTK4 / AppKit / Win32) with an
  AetherUIDriver HTTP test server. Was `contrib/aether_ui/` in this
  repo until the spin-out; now consumes Aether the same way external
  users do (install + `$(ae cflags)`). Useful reference for the
  embedded-DSL pattern: the toolkit's surface IS a closure-DSL.
- **aeb** (`https://github.com/aether-lang-org/aeb`) — multi-package
  build system. Reads `share/aether/MANIFEST` to discover link-suitable
  runtime/stdlib `.c` files and orchestrates per-package compile +
  cache + incremental relink. The MANIFEST contract (`docs/install-
  layout.md`) was carved out specifically to support aeb without
  forcing it to guess via `find -name '*.c'`. If you're touching
  install-layout / shipped source / link contract, ping aeb side.
- **Feature request flow that works**: downstream writes a spec
  (e.g. `import_typer_at_scale.md`, `exprt_structs.md`,
  `stdlib_wish.md`), Aether implements, downstream adopts within the
  same day. The specs are extremely concrete: API names, signatures,
  rationale, call-site census. Match that level when responding.
- **Don't gate on things that aren't real threats.** `--emit=lib`
  capability-empty is right for the embedded-DSL case (host accepts
  untrusted Aether). avn / aether-ui / aeb are the opposite case —
  they are hosts. The `--with=fs` opt-in covers both cleanly.
  Similar dualities will come up; watch for them.
- **Downstream embedding link line: ALWAYS `$(ae cflags)`.** Don't
  hand-craft `-I` / `-L` / `-laether`. The install layout
  (`<prefix>/lib/aether/libaether.a`) requires an explicit
  `-L<prefix>/lib/aether` that bare `-laether` won't find, and the
  include path varies between dev/user/system installs. `ae cflags`
  emits the right flags for whichever install is in effect.

## Build / test commands

- Full build: `make` (from repo root). Rebuilds `build/aetherc` and
  `build/ae`. Incremental builds sometimes miss `aetherc` reshapes;
  `make clean && make -j$(nproc)` when in doubt.
- All regression tests: `make test` or `make check`. Hot inner loop
  during feature work: `make test-ae` (parallel runner for
  `tests/regression/*.ae`).
- JSON conformance: `make test-json-conformance` — must pass 95/95
  `y_*` + 188/188 `n_*` for JSONTestSuite.
- Sanitizers: `make test-json-asan` / `make test-json-valgrind`.
- **Version drift after fetch.** Makefile derives `AETHER_VERSION`
  from the highest `v*.*.*` git tag, falling back to `VERSION` file.
  If `aetherc --version` disagrees with CHANGELOG `[current]` or
  with `cat VERSION`, the local tags are behind: `git fetch --tags`,
  then `make clean && make`.
- **Coverage path is gcc `--coverage` + gcov, no custom mapper.**
  As of PR #352 codegen emits `#line N "src.ae"` directives, and
  gcov reads them natively — so `gcc --coverage` on a generated .c
  produces a `src.ae.gcov` (line + branch hits against .ae source)
  with no extra plumbing. When wiring `make ci-coverage` (Phase 2),
  the work is just adding `-fprofile-arcs -ftest-coverage` to the
  build (mirror the `build/asan-obj/` variant pattern), running the
  tests, and shelling `gcov` over the .gcda files.

## Branch / PR conventions

- Prefix with `feat/` for features, `fix/` for bug fixes, `docs/`
  for doc-only. Examples in merged history: `feat/json-object-iteration`,
  `fix/parser-ergonomics-and-cflags`, `fix/block-scope-restoration`.
- CHANGELOG entries go under `[current]` as part of the PR. The
  release workflow renames `[current]` → `[X.Y.Z]` at tag time.
- Never commit to `main` directly. Push the feature branch, open a
  PR, wait for green CI (includes Windows MSYS2 + MINGW-w64
  cross-builds that catch `#include` gaps Linux doesn't).

## Invariants to not break

- `--emit=lib` stays capability-empty by default. `--with=` is the
  only escape hatch; don't add backdoors (per-function allowlists,
  `--with=all`, etc.).
- The `aether_<name>()` ABI mangling is stable across releases. If
  you rename an export, the old name stays as an alias.
- `std.string.from_int(int)` and `string.from_long(long)` are
  separate — don't merge. `long` is 64-bit on every target including
  MSVC (where C `long` is 32 bit) because Aether's `long` type is
  defined as `long long` at the C level.
- Iteration order in `std.json` is insertion order across both parse
  and builder paths. Documented contract, several downstream users
  (including svn-aether's server) rely on it.
- **Any C symbol codegen emits a call to must be linkable in EVERY
  build, including WASM.** The `ci-wasm` Makefile target uses its own
  minimal `RUNTIME_FILES` list (not `RUNTIME_SRC`); add new runtime
  files there too, and keep the symbol self-guarded so it's a no-op
  object off its target OS. Otherwise `wasm-ld` errors `undefined
  symbol` while every native platform is green. (Bit us when the
  Capsicum self-sandbox startup hook landed.)

## When stuck

- `git log --oneline -30` to see what just shipped.
- Search tests: `grep -rn "capability\|emit.lib\|with=" tests/`.
- Check `docs/` first, then `compiler/` for language-level questions,
  then `std/<module>/` for runtime.
- The CHANGELOG has working-memory for what-landed-when. Treat it as
  authoritative for "is feature X available yet."
