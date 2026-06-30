# Changelog

All notable changes to Aether are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

**Workflow**: New changes go under `## [0.331.0]`. When a PR merges to
`main`, the release pipeline automatically replaces `[current]` with the
next version number before tagging the release.

## [0.331.0]

### Added

- **Optionals: `T?` with `none`, `!`, `??`, `?.`, and `match`** (#340). A
  first-class optional type for "maybe a value," complementing the `(value,
  err)` result convention (which stays the tool for *fallible* operations).
  `T?` collapses the ambiguous "is the value a null pointer, or is the key
  absent?" case (`map.get`, `list.first`, a search that found nothing) to one
  type with predictable handling. Surface:
  - `let m: int? = 69` wraps a value; `let z: int? = none` is the empty
    sentinel. `none` is a reserved literal (like `true`/`false`/`null`) and
    cannot be a variable name. `== none` / `!= none` test presence.
  - Force-unwrap `m!` yields the value or panics on `none` (`forced unwrap of
    \`none\``). Null-coalesce `m ?? d` yields the value or `d`, and binds
    tighter than arithmetic. Optional chaining `v?.field` is none-propagating
    (yields `fieldT?`); chain assignment `v?.field = x` is a no-op when `v` is
    `none`.
  - `match m { none -> …  some(v) -> … }` destructures as a statement or an
    expression. A bare `T` (or `none`) is implicitly wrapped into a `T?`
    parameter, return value, or binding.
  - One uniform representation covers value and reference element types
    (`typedef struct { int has; T val; } ae_opt_<T>`), so there is no
    null-vs-absent ambiguity. Postfix `!` is polymorphic on its operand — an
    optional unwraps the value, a `(value, err)` tuple unwraps the first slot —
    so it does not collide with the actor-send `!` (which is followed by a
    message type) or with `match` pattern arms. See the
    [language reference](docs/language-reference.md#optionals).

### Fixed

- **`ae build` now fails on an imported module's compile error** (#953). `ae
  build` accepted an entry program whose *imported* module did not compile —
  the parser's error recovery dropped the offending construct (e.g. an invalid
  `@` annotation lowering to a bare `return x`), so the merged AST type-checked
  clean and codegen produced a working binary from non-compiling source, while
  `ae check` correctly reported the error. The two disagreed on validity, and a
  build's exit code couldn't be trusted (it bit a mutation-testing driver that
  rebuilds an imported SUT). The entry file's own parse errors were already
  gated, but the global error count was not re-checked after module
  orchestration — which is where imported modules are parsed. Both `build` and
  `check` now fail (non-zero, no binary) when any module they pull in carries an
  error. A clean import is unaffected (the gate keys on the error count).

- **Heredoc closing-marker rule: no more silent truncation** (#922). A heredoc
  body line that merely read like the closing marker could close the heredoc
  early and silently drop the rest of the body. The close rule is now: a line
  closes the heredoc only when it is the marker alone on its line AND its
  indentation is at or below the shallowest body line — the terminator lives at
  the content's base level. A more-indented marker-like line is therefore body
  content (never a silent truncation); a lone marker indented *past* the body
  matches nothing and is reported as an unterminated heredoc rather than
  dropping content. The closing marker may still be indented (at/below the body
  base; column 0 always works), the marker must be alone on its line
  (`done END` / `xEND` stay content), and body dedent is unchanged (common
  leading-whitespace / least-indented line, like Ruby's squiggly `<<~`). Docs
  (`LLM.md`, language-reference) corrected — they wrongly claimed "column 0
  only," which the lexer never enforced.

- **Qualified type name `mod.Type` accepted in type positions** (#946). A
  module-qualified name was accepted as a value/call (`lib.mk(...)`, #878) but
  not as a *type* — the parser stopped at the `.` (`Expected RIGHT_PAREN, got
  DOT` in a parameter, `Expected LEFT_BRACE, got DOT` in a return type). Only
  the bare exported name worked, which left no way to disambiguate when two
  imported modules export a type with the same name. `mod.Type` now parses in
  parameter types, return types, and C-style typed locals (`mod.Type name`),
  resolving to the bare exported type (the merge brings an exported struct
  into the consumer's namespace unprefixed, so the qualifier is a
  disambiguator). The type parser accepts a dotted name; the return-type
  disambiguator and the typed-local statement dispatcher were taught the
  dotted-name shape so they route to it. (Using an imported struct as a
  *struct field* remains a separate, pre-existing limitation that affects the
  bare name equally — incomplete-type in the consumer TU — and is unrelated to
  this parser asymmetry.)

## [0.330.0]

### Fixed

- **Bare top-level function used as an `fn` value inside a closure body**
  (#943, closure analogue of #940). Wrapping a bare named function as an
  `fn` value from inside a trailing-block closure (`runit(val)` inside a
  `callback { ... }`) failed to compile: the emitted closure function
  referenced an `_aether_bare_adapter_<name>` shim that was only *defined*
  later in the file, so it was undeclared in the closure's translation unit.
  The cause was emit order — closure bodies were emitted before the bare-fn
  adapters, but a closure body can itself wrap a bare fn. The adapters' C
  forward declarations are now emitted before the closure definitions (the
  full bodies still follow, since they call the user functions by name), so
  closure bodies see the prototype in scope. Works in combination with #940
  (a bare fn wrapped inside a closure whose callee is an imported function).

## [0.329.0]

### Fixed

- **Bare top-level function passed as an `fn` arg across a module boundary**
  (#940). Passing a bare named function as an `fn`-typed argument to an
  *imported* module's function failed to compile — the caller referenced an
  `_aether_bare_adapter_<name>` env-ignoring shim that was never emitted in
  the caller's translation unit. The adapter-discovery pre-walk looked up the
  call's callee by its AST name, which for a qualified `mod.fn(...)` call is
  still dotted (`runner.runit`) while the merged definition is `runner_runit`;
  the lookup missed, the `fn`-typed parameter was never inspected, and the
  bare-fn argument's adapter was never registered. The lookup now also tries
  the merged (`.`→`_`) form, so a library API that takes a caller-supplied
  callback by bare name (visitors, comparators, retry/poll predicates) works
  across the import boundary — same as it already did single-file.

## [0.328.0]

### Fixed

- **Module-level `var` (#701) now persists across the import boundary**
  (#937). A mutable module-level `var` defined in an *imported* module lost
  writes: a store inside one of the module's functions was visible to that
  function (it returned the written value) but a later call into the same
  module read the initializer back (`write-returned=7  read-back=0`). The
  module-merge's intra-module rename rewrote *reads* of the global to its
  prefixed name but not the *write target* of an assignment — and worse,
  counted a bare `name = expr` write as a function-local, which shadowed the
  global out of renaming entirely. Codegen then emitted a throwaway local
  (`int counter = n;`) instead of a store to the shared `static`, so the
  write never reached the cell. The rename now treats a bare-name write to a
  module global as the global it is (not a local declaration) and rewrites
  the assignment target, so the store reaches the shared cell — the
  "ambient context / process-global provided by a library" pattern (a config
  cell, a registry, a current-context set during init and read later) works
  across imports. Genuine same-named locals are unaffected.

## [0.327.0]

### Added

- **First-class module re-export** (#924). A module may now list, in its
  `exports`, a symbol it brought in via `import` — and that symbol becomes
  part of its own qualified surface, identically to one it defined
  (`hub.X` resolves to the defining module's symbol). Re-export is transitive
  (a facade can re-export through several layers) and visibility still gates
  it (the origin must export the name). A locally-defined export always wins
  over a same-named import, so there's no ambiguity. This dissolves the
  facade-monolith and per-consumer extern re-declaration patterns: a large
  constants/API module can be decomposed into cohesive leaves that a thin hub
  re-exports, with consumers' `import hub` unchanged — and it breaks the
  `hub → leaf → hub` import cycle that derived-constant leaves otherwise force.

- **UFCS resolves across the import boundary** (#934, follow-up to #928).
  `value.method()` now finds a `method` exported by an imported module whose
  first parameter matches `typeof(value)`, honoring the same visibility as a
  normal qualified `mod.method(value)` call — not just same-file functions.
  This is what makes library-provided fluent surfaces work: a test framework's
  `expect(x).to_equal(5).to_be_gt(0)` with the matchers in an imported module
  and the chain in the consumer's file. Same-file functions still take
  priority; a type-mismatched receiver declines cleanly.

### Changed

- **Circular-import diagnostic names the actual cycle** (#925). The error now
  reads `circular import dependency: a -> b -> a`, listing the participating
  modules in order, instead of the prior `involving module '__main__'` at a
  bogus `0:0` (the synthetic entry root, which is never part of a real import
  cycle). In a large module tree this turns "a cycle exists somewhere — go
  find it" into an actionable trace.

## [0.326.0]

### Added

- **Method-call-on-value (UFCS)** (#928). `x.f(args)` now desugars to
  `f(x, args)` when `f` is a free function whose first parameter type matches
  `typeof(x)` — the missing primitive for fluent / method-chaining DSLs
  (`expect(5).to_equal(5)`, `subject.inc().to_equal(6)`). It works on any
  receiver expression: a call result, a stored value, or a pointer
  (`c.bump()` → `bump(c)` for `c: *Counter`). UFCS is a strict **last-resort**
  fallback — module-qualified calls (`string.length(s)`), struct-field access,
  and function-pointer-field dispatch all keep priority, so nothing that
  compiled before changes meaning; UFCS only fires on a dotted call that would
  otherwise be an "Undefined function" error. A receiver whose type doesn't
  match the candidate's first parameter declines cleanly (no silent coercion).
  No new declaration syntax and no codegen change: existing free functions
  become chainable, and the rewritten call lowers like any other by-value
  call.

## [0.325.0]

### Fixed

- **Module-scope `var` now honours the silent-narrowing guard** (#929). A
  module-scope `var x = 0` infers a 32-bit `int` from its bare initializer,
  exactly like the local `x = 0` form — but the #698 narrowing guard (E0200)
  only fired on locals, so a later 64-bit assignment to the global
  (`x = os.now_monotonic_ns()`) truncated silently. The parser now marks the
  global's inferred type and the typechecker carries that marker onto the
  symbol, so the assignment raises E0200 with the same "annotate the
  declaration" suggestion. An explicit width (`var x: long = 0`) is exempt, and
  a plain int global assigned int values is unaffected.

- **Multiple `${duration}` interpolations in one string render distinct values**
  (#927). The codegen helper `_aether_duration_repr` returned a shared static
  buffer, so when several durations appeared in a single interpolated string
  (`"${a} ${b} ${c}"`) all `%s` slots pointed at the last-formatted value —
  every slot printed the same duration. The helper now hands out a small ring of
  buffers, so up to eight distinct durations coexist in one printf/snprintf.

## [0.323.0]

### Added

- **Labeled `break` / `continue`** (#893). A `while` / `for` loop can carry a
  label — `outer: while ...` — and `break outer` / `continue outer` then target
  that loop from inside a nested loop (`break` exits it, `continue` jumps to its
  next iteration). This replaces the boolean-flag emulation a faithful C port
  otherwise needs for a nested-loop early exit (the `goto cleanup` idiom). The
  label must be on the same line as the `break`/`continue`; a label naming no
  enclosing loop is a compile-time error; defers in the unwound scopes still run
  (LIFO) before the jump. Unlabeled `break`/`continue` are unchanged.

## [0.321.0]

### Changed

- **Qualified `mod.fn()` surface is available on any import form** (#878). A
  module's qualified call surface (`string.length()`, `math.pow()`) now resolves
  whenever the module is imported in *any* form — bare, selective, or glob —
  like Java's always-legal fully-qualified name. A selective import
  (`import std.math (sqrt)`) is now purely additive: it adds the bare-name
  binding `sqrt(...)` on top of the always-available qualified surface, instead
  of restricting it. Previously a selective import rejected the qualified form
  of any non-selected name (`math.pow` failed under `import std.math (sqrt)`),
  which forced real code to import a module twice (once selective, once bare).
  The per-module selective filter that enforced that restriction is removed;
  export visibility (`exports (…)`) and `hide`/`seal` still gate qualified
  access.

### Fixed

- **Imported `distinct` types now resolve across the module boundary** (#908).
  A `type X = distinct Base` defined in an imported module was never merged into
  the consumer's program, so the distinct-resolution pass never learned `X` —
  every cross-module `expr as X` / `x as Base` failed (`cannot cast X to Base`)
  and codegen emitted an unknown C type `X`. The bug surfaced via the builder-
  child (`_ctx`) path but was broader: any cross-module distinct wrap/unwrap was
  affected. The module merge now pulls imported `distinct` defs into the program
  (bare name, like struct defs), at both the direct-import and transitive-pull-in
  sites, so every reference resolves.
- **Heap double-free returning a string-field struct in a tuple** (#911). A
  `-> (StructWithStringField, err)` constructor whose field was initialized from
  a string variable double-freed at runtime (`free(): invalid pointer`): the
  struct literal hard-coded `._heap_<field> = 1`, claiming ownership even when
  the source variable held a *borrowed* string (`e = s`), so the struct's
  owned-field free ran on a pointer it never owned. The field's heap-ownership
  flag now mirrors the source variable's runtime `_heap_<v>` flag, and the
  variable is disowned (move) so its deferred free is a no-op — exactly one free,
  ASan/leak-clean for the genuinely-heap case. Unblocks the idiomatic "parse a
  record at the boundary, return `(Record, err)`" shape.

## [0.320.0]

### Added

- **`@c_struct` typed overlays — width-correct C-struct field access over a
  raw `ptr`** (#891). Declare a C struct's layout once with explicit offsets
  (`@c_struct stream { length: uint64 @8, slen: uint32 @16, last_id: streamID
  @24 }`); then `ptr as *stream` views a raw pointer through it and `s.length`
  / `s.slen` / `s.last_id.ms` read and write by name. The **accessor width is
  derived from the field type** (`uint32`→4 bytes, `uint64`→8, `ptr`→pointer-
  width, …), so the hand-picked-width footgun behind #868 (a `uint32` read with
  `get_long` pulling adjacent bytes) is gone structurally — the compiler never
  lets you pick the wrong width. Nested overlays add offsets along the chain
  (`s.last_id.seq` → 24+8). It is a pure-Aether lens: no `extern struct`, no
  C struct emitted, no `#include`, no `import std.mem` — it lowers to
  `aether_mem_*` calls over a `void*`, and the C side keeps owning the memory.
  Reuses the existing `expr as *Name` cast and `s.field` syntax. (See
  docs/c-interop.md “`@c_struct` typed overlays”.)
- **`aetherc --emit=effects` — derived per-function effect/purity JSON** (#889).
  Exposes the whole-program effect analysis (#481/#522) for external auditors
  (aeb’s supply-chain veto): `{ "<fn>": { "pure": bool, "extern": bool,
  "reaches": ["fs","net","os"] } }` on stdout (peer of `--emit=ast`/`inspect`,
  no codegen). The result is **derived** from the call graph — not author
  `@no_*` tags an attacker could omit — whole-program transitive (through
  helpers *and* imported modules), per-function, and fail-closed on a raw
  `extern` (treated as reaching every capability, never pure), matching the
  `--with=` gate’s boundary.

## [0.317.0]

### Fixed

- **Glob-imported symbols now resolve across a module boundary** (#896). A
  module that used `import M (*)` and called a glob-brought symbol compiled
  standalone but failed with `Undefined function` once it was imported by
  another module — the merger skipped glob imports when rewriting a consumed
  module's bare references to their prefixed form (only selective/qualified
  imports were rewritten). The merge-time rewrite now treats a glob import's
  selection as the imported module's full export set, so a bare `clean(...)`
  in the consumed module's body lowers to `fs_clean(...)` exactly as the
  selective and qualified forms already did.

## [0.316.0]

### Added

- **`sizeof` / `offsetof` in `const` initializers** (#879). The two layout
  builtins are now accepted in a top-level `const` initializer (and arithmetic
  over them) — `const SIZEOF_T = sizeof(T)`, `const OFF = offsetof(T, field)`,
  `const PAD = sizeof(T) + 8`. They lower to C compile-time constant
  expressions, so a port that mirrors C structs as `extern struct` overlays can
  centralise its offset/size table as named consts that are self-verifying by
  construction (the C compiler folds each value) instead of hand-maintaining
  numbers plus `_Static_assert` drift guards. The general "no function calls in
  a `const` initializer" rule is unchanged; these two builtins are the carve-out.

- **Type/keyword tokens usable as value identifiers** (#880). `ptr`, `byte`,
  `func`, `state` and `after` can now be used as ordinary value identifiers —
  parameter names, local names, struct field names, struct-literal fields, and
  field-access targets — without the `` `name` `` backtick escape. These tokens
  have meaning only in type / declaration-head / statement-head position, so a
  bare occurrence in value position is unambiguously a name. A C→Aether port no
  longer has to rename `ptr`→`ptr_`, `func`→`fn_val`, etc. (`match` and `union`
  stay reserved — `match` heads a match expression; `union` is a C keyword that
  can't be emitted as a C identifier — use the backtick escape for those.)

## [0.315.0]

### Added

- **Address-of operator `&lvalue`** (#890). Prefix `&` takes the address of an
  lvalue and lowers to C's `&` — `&(p as *T).field` → `&((T*)p)->field`,
  `&local.field` → `&local.field`, plus `&local` / `&arr[i]`. The result is a
  pointer (assignable to a `ptr` parameter), so a C extern with a
  `&struct->field` out-param (in-place mutation, sub-field write, resize
  destination) is callable without raw `mem.long_to_ptr(base + OFFSET)` offset
  math.

- **Array-to-pointer decay in pointer context** (#892). A named fixed-size
  array decays to a pointer to its first element when used as an inferred
  binding initializer, a `ptr`-typed argument, or in a pointer comparison
  (C semantics). `ids = static_ids` (with `byte[128] static_ids`) infers `ids`
  as a `ptr`, so a later `ids = heap` / `ids = null` stays legal — the
  stack-buffer-with-heap-fallback idiom. An array *literal* (`x = [1,2,3]`)
  still binds a real array; annotate explicitly to keep the array type.

- **Distinct types: `type Name = distinct Base`** (#480). A zero-cost nominal
  wrapper over a scalar / `string` / `ptr` base — `type USD = distinct float`,
  `type Fd = distinct int`. Lowers to the base C type (no boxing), but the type
  checker treats it as nominally separate: crossing the boundary needs an
  explicit `as` cast (`9.99 as USD` to wrap, `usd as float` to unwrap; `as`
  also does ordinary numeric conversions). Enforced at variable
  declarations/assignments and at call-argument boundaries (a `Fd` parameter
  rejects a raw `int`; an `EUR` is rejected where `USD` is wanted) — the
  capability-token discipline now compiler-checked.

## [0.314.0]

### Added

- **Gradual contracts: `where` clauses on function parameters** (#525). A
  parameter may carry a runtime-checked precondition: `divide(a: int, b: int
  where b != 0)`. It lowers to the same entry guard as `requires` — a violation
  is a hard panic naming the condition (`precondition violation: b != 0 in
  divide`), a programmer-error signal, not a recoverable `(value, err)`. Opt-in
  and gradual: a parameter with no `where` is unchecked; multiple `where`
  params and `and`-composed conditions are allowed; suppressed by
  `--no-contracts` like the other contract checks. (`where` on bindings is a
  tracked follow-up — it needs a binding-syntax decision, since Aether bindings
  are prefix/inferred, not the issue's postfix `let x: T` form.)

## [0.313.0]

### Added

- **Static purity inference + the `__pure(fn)` builtin** (#522). A whole-program
  analysis classifies each function pure/impure: pure means it transitively
  reaches no fs/net/os capability call and mutates no caller-visible state (a
  parameter's pointee or a module global). The compile-time `__pure(funcName)`
  builtin folds to a `true`/`false` constant, so code can branch on purity at
  compile time. Conservative — an extern / unresolved function is treated as
  impure. Reuses the #481 call-graph + capability classification.
- **Per-function effect tags: `@pure` / `@no_fs` / `@no_net` / `@no_os`** (#481).
  A function annotated with an effect tag declares it must not (transitively)
  use the named capability; `@pure` forbids all of fs/net/os. A whole-program
  pass walks the call graph from each tagged function and errors if a forbidden
  capability is reached — e.g. `@no_fs load(...)` calling `file.read_all(...)`,
  directly or through another function. Composes with the build-time
  `--with=fs,net,os` gate (whole-program) as a finer, per-function axis. A raw
  `extern` call is unclassifiable and is not flagged, matching the `--with=`
  gate's boundary.

## [0.312.0]

### Added

- **`@scoped` bindings — opt-in escape analysis** (#521). A `let`/`var`
  declaration annotated `@scoped` (`@scoped let buf = make_buffer()`) declares
  that the value must not outlive its lexical block. The typechecker rejects
  every escape: returning the binding, aliasing it into another binding or
  field, placing it in an aggregate literal, capturing it in a closure, or
  inserting it into a container (`list.add`/`map.put`/…). Only a scalar
  *derived* from it may escape (`return buf.len()`). Not a borrow checker —
  one opt-in annotation that turns a non-escape into a checked invariant.

## [0.311.0]

### Added

- **Raw identifiers: `` `name` `` escapes a reserved keyword for use as an
  ordinary identifier** (#867). A backtick-delimited identifier is always
  lexed as a plain name, so a faithful C→Aether port can keep identifiers
  like `` `reply` ``, `` `message` ``, `` `after` ``, `` `ptr` ``, `` `when` ``
  as parameter, local, struct-field, or function names instead of renaming
  every site. The parameter-position diagnostic for an *unescaped* reserved
  keyword now points at the keyword and teaches the escape (previously a
  misleading "Expected RIGHT_PAREN").
- **`heap.new(T)` supports structs with `string` fields** (#790). The POD-only
  restriction is lifted: a heap-boxed struct now owns its string fields under
  the same model value structs use — a field store adopts the heap string (and
  frees the previous one on reassignment), and `heap.free(p)` releases every
  owned field before freeing the box (a borrowed literal is never freed). The
  `calloc` in `heap.new` zero-inits the ownership trackers. This closes the
  handler-context gap (`struct AppCtx { db: ptr; data_dir: string }`) so such
  contexts no longer need a raw `malloc(...) as *T`.
- **`aetherc --audit-mem`** (#868): lists every raw `std.mem` offset access
  (`mem.get_*`/`mem.set_*`) with the byte width its accessor name implies, then
  exits without generating code. Lets a port author audit each read/write width
  against the C field's actual type — the width-exact accessors already exist,
  but nothing previously surfaced a wrong choice (reading a 4-byte field with
  `get_long` pulled in adjacent bytes).

### Fixed

- **An explicitly-typed integer local keeps its declared width across a bare
  re-bind** (#869). `uint64 v = 0` followed by an annotation-less `v = <int
  expr>` no longer silently re-infers `v` to 32-bit int (the re-bind parsed as
  a fresh declaration and adopted the initializer's type), which previously
  discarded the explicit width and tripped the #698 narrowing guard at the next
  64-bit assignment. The explicit declaration is now authoritative — an int RHS
  widens into the declared type. Fixes silent truncation in the `string2ll`
  accumulator shape (every 10+-digit integer parse).
- **A selective `import std.string (...)` no longer suppresses qualified
  `string.X` calls from merged modules** (#870). When the entry file imported a
  module selectively, the module-merge dropped the bare/non-selective surface
  for the whole compilation unit, so a qualified `string.concat(...)` arriving
  from an imported module that bare-imports `std.string` was rejected with
  E0301. The merge now injects a synthetic bare import for each merged module's
  own bare imports, re-opening the qualified surface (kept out of the
  user-explicit registry, preserving #243 sealed-scope isolation).

## [0.310.0]

_No user-facing language/stdlib changes recorded for this release; see git
history for internal/infra commits._

## [0.309.0]

_The entries previously listed here were misattributed: they shipped across
0.311–0.316 and have been moved to their correct release sections above. See
those sections for the real 0.311–0.316 notes._

## [0.308.0]

### Fixed

- **Module-level mutable global of `string` type now writes the static, not a
  local shadow** — a bare `name = expr` inside a function body assigning to a
  `#701` module-level `global_var` string lowered to a shadowing local instead
  of the file-scope static, so the write was lost. It now resolves to the
  module static. (Part of #861.)

## [0.307.0]

### Added

- **`--emit=lib` now exports module-level `const` declarations** (#854). A
  `--emit=lib` artifact's `aether_lib_meta()` catalog carried functions and
  closures but not module-level constants, so a consumer importing the `.so`
  (no source) failed every `foo.SOME_CONST` reference. Exported scalar/string
  consts (`int`, `long`, `bool`, `float`, `string`) now cross the boundary:
  they're recorded in the catalog (schema **1.2**, forward-compatible — a
  1.0/1.1 reader ignores the new slot) and rehydrated as `const NAME = value`
  in the synthesized binimport stub, so `foo.SOME_CONST` resolves against a
  `.so` exactly as against source, with no call-site changes. `ae lib-info`
  gains a `Constants:` section. Function-only artifacts stay byte-identical at
  schema 1.0. Typed const *arrays* (#745) remain out of scope (skipped, never
  half-emitted).

### Changed

- **Clearer diagnostic for a non-exported module member** (#854). Referencing
  a name an imported module doesn't export (e.g. a constant absent from a
  `.so`'s ABI) reported the misleading `Undefined variable '<module>'`, which
  pointed at the module rather than the member. It now reports
  `error[E0200]: module '<module>' has no export '<NAME>' (not part of the
  module's API / library ABI)`. Scoped to the value/member path; non-exported
  *function* calls already named `<module>.<fn>` and are unchanged.

## [0.306.0]

### Added

- **Embedded Racket and Rhombus host modules** — `contrib.host.racket` and
  `contrib.host.rhombus` embed the Racket CS runtime in-process with a live,
  persistent VM (#852). Racket and Rhombus are the **same VM** (Rhombus is a
  `#lang` on the Racket runtime), so one shared bridge backs both surfaces and
  they share one persistent VM and one string-only k-v map (a key set via
  `racket.set` is read via `rhombus.get`). Surface mirrors the other hosts:
  `evaluate` / `run` / `set` / `get` / `run_sandboxed` /
  `run_sandboxed_with_map` (live shared-map interop) / `init` / `finalize`.
  - **No fork, no patches** — unlike `contrib.host.factor` (which needs a
    forked libfactor), both upstreams are used as-shipped: Racket via a stock
    `make cs` build (it exposes a first-class embedding API), Rhombus via
    stock `raco pkg install rhombus`.
  - **Static-linked, not dlopen** — Racket CS has no shared `libracketcs`
    (upstream refuses `--enable-shared`), so a program importing the bridge
    static-links `libracketcs.a` (from `$AETHER_RACKET_LIB`) plus the runtime's
    system deps; the VM boots from the petite/scheme/racket boot images in
    `$AETHER_RACKET_BOOT_DIR`. The result-returning call is `evaluate` (not
    `eval`) because `libracketcs.a` exports its own `racket_eval`.
  - Experimental and **not in the default `CONTRIB_HOST_LANGS` set** (needs a
    built Racket CS); `make contrib` SKIPs the archive when the embedding
    headers aren't present. Same sandbox caveat as `host/factor`: the VM's own
    GC/JIT/threads aren't contained by the libc gate — rely on the
    process-level sandbox. See `contrib/host/racket/README.md`.

## [0.305.0]

### Added

- **Post-quantum ML-KEM, more NIST curves, and two block ciphers** — a large
  pure-Aether crypto tranche of the Bouncy Castle port (#739), no externs to
  OpenSSL.
  - **`std.cryptography.mlkem`** — ML-KEM / Kyber (FIPS 203), all three
    parameter sets (512/768/1024): `mlkem{512,768,1024}_{keygen,encaps,decaps}`.
    NTT over Z_3329 with Montgomery/Barrett reduction; SHAKE128/256 sampling
    reusing `std.cryptography.sha3`. Aether's first post-quantum primitive.
    Validated **byte-exact against NIST ACVP** vectors: keyGen (ek/dk) and
    encapDecap (ciphertext + shared secret) known-answers for all three
    parameter sets, plus the implicit-rejection (VAL) path. The committed
    integration test pins the ML-KEM-512 keyGen KAT (tcId 1) and the KEM
    round-trip on all sizes.
  - **`contrib.cryptography.p384` / `p521`** — NIST P-384 and P-521 ECDH +
    ECDSA, parameter-swaps of the existing P-256 short-Weierstrass module.
    Validated against the published 2·G doubling vectors + ECDSA round-trips.
  - **`contrib.cryptography.sm4`** (GB/T 32907) and **`contrib.cryptography.des3`**
    (3DES / TDEA) block ciphers, with the same ECB/CBC/CTR + PKCS#7 mode layer
    as `aes`. Validated against the SM4 GB/T 32907 KAT and a BC 3DES KAT.

### Changed

- **`std.bignum` performance** (#233) — internal Karatsuba multiplication (for
  large operands) and Montgomery `mod_pow` (for odd moduli, the RSA/DH hot
  path), with the previous schoolbook code retained as the fallback. Public
  API and all results are unchanged — purely faster. A 2048-bit `mod_pow`
  drops from ~17 s to ~0.2 s (~90×); speeds up RSA and every elliptic curve.
- **`std.cryptography` digests now use `std.bytes.get_le64`/`set_le64`** instead
  of hand-rolled little-endian 64-bit byte assembly (blake2, skein, tiger,
  sha3, argon2 — 12 sites). Behavior-preserving; follow-up to #838.

## [0.304.0]

### Documentation

- **Crypto digest context-ownership contract (#837) is now documented
  consistently across every streaming hash module.** `final_hex` /
  `final_bytes` free the context; `free_ctx` is only for abandoning a
  context *before* finalizing — calling `free_ctx` after a successful
  `final_*` is a double-free. Previously only `std.cryptography.sha2`
  stated this. The rule is now on the `final_*` / `free_ctx` doc-comments
  and in the header usage example of `sha3`, `sm3`, `blake2`,
  `ripemd128` / `ripemd160` / `ripemd256` / `ripemd320`, `whirlpool`,
  `tiger`, and `skein`, and the streaming examples that previously invited
  the broken pattern now carry an explicit `ownership:` note. Comment-only;
  no behavior change.

## [0.302.0]

### Changed

- **Caps-audit (#462): the `std.fs` file handle is now memory-cap
  accounted.** `file_open_raw` routes both the `File` struct and its
  retained path copy through `aether_caps_malloc` (a sandboxed caller can
  craft an enormous filename to inflate filesystem-driven memory), and
  `file_close` releases both with their exact sizes so the accounting
  returns to baseline. The large-file read buffer was already cap-bounded
  (#343/#463). Added two runtime tests: `caps_fs_file_open_close_balances`
  (open + close round-trips to baseline, no accounting drift) and
  `caps_fs_read_denied_past_cap` (#462's acceptance case — a read whose
  buffer exceeds the sandbox's remaining headroom is refused with the
  counter intact). Returned heap strings (path_join/clean/rel results)
  remain plain `malloc` by design — the Aether heap-string machinery owns
  and frees them, so cap-allocating them would drift the counter.
## [0.301.0]

### Added

- **`std.bytes` little-endian 64-bit accessors** — `set_le64(b, index, value)`
  / `get_le64(b, index)`, completing the LE/BE × 16/32/64 matrix (the only
  cell that was missing; `be64` and `le32` already existed). Mirrors the
  `be64` shape: grow-on-write, `-1` on out-of-range read, lossless round-trip
  for any `long`. Removes the hand-rolled byte-by-byte LE64 word assembly that
  6+ crypto modules (Keccak/SHA-3, BLAKE2b, Salsa20/scrypt, Argon2, Skein/
  Tiger, X448/Ed448) each reimplemented. Regression in
  `tests/regression/test_bytes_le64.ae`. (#838)

## [0.300.0]

### Added

- **AEAD, password hashing, more curves, and classic hashes** — a large
  pure-Aether tranche of the Bouncy Castle port (#739), each validated
  against NIST / RFC / Bouncy Castle test vectors. No externs to OpenSSL.
  - **`contrib.cryptography.aes`** gains three more AEAD modes on the
    existing block primitive: **CCM** (NIST SP800-38C), **EAX** (over the
    existing CMAC), and **OCB** (RFC 7253) — `ccm_seal`/`ccm_open`,
    `eax_seal`/`eax_open`, `ocb_seal`/`ocb_open`, each with a constant-time
    tag check and `-1` failure sentinel on tamper.
  - **`std.cryptography.scrypt`** (RFC 7914) and **`std.cryptography.argon2`**
    (RFC 9106 — Argon2d/i/id) password-hashing KDFs. scrypt reuses PBKDF2;
    Argon2 reuses BLAKE2b. `scrypt(...)`, `argon2id`/`argon2i`/`argon2d`
    (+ `_hex`), with optional secret/AD.
  - **`contrib.cryptography.secp256k1`** (Koblitz curve — ECDH + ECDSA, the
    same short-Weierstrass plumbing as P-256), **`contrib.cryptography.x448`**
    (RFC 7748 Montgomery ladder), and **`contrib.cryptography.ed448`**
    (RFC 8032 — SHAKE256-based, 57-byte keys / 114-byte signatures).
  - **`std.cryptography.whirlpool`** (ISO/IEC 10118-3), **`std.cryptography.tiger`**
    (192-bit), and **`std.cryptography.skein`** (Skein-512, Threefish + UBI),
    each with the sm3-style streaming + one-shot API.
  - Correctness-first ports over the variable-time std.bignum (curves) — not
    constant-time/side-channel-hardened; documented in each module header.
    Skein-256/1024 state sizes and Tiger2 are noted as deferred.
- Integration tests: `tests/integration/crypto_{aead_modes,pwhash,classic_hashes,curves2}`.

## [0.299.0]

### Added

- **SHA-3 / SHAKE (FIPS 202)** — `std.cryptography.sha3`, the Keccak hash
  family in pure Aether ported from Bouncy Castle (#739), no externs to
  OpenSSL. Keccak-f[1600] permutation (θ/ρ/π/χ/ι, 24 rounds) under a sponge:
  fixed-length `sha3_{224,256,384,512}_{hex,bytes}` and the extendable-output
  functions `shake{128,256}_{hex,bytes}(data, len, out_len)`, plus a streaming
  `new(variant)` / `update` / `final_*` ctx. Validated against FIPS 202 /
  NIST known-answer vectors (SHA3-224/256/384/512 and SHAKE128/256).
- **BLAKE2b + BLAKE2s (RFC 7693)** — `std.cryptography.blake2`, pure Aether
  from the Bouncy Castle port (#739). 64-bit BLAKE2b (≤64-byte digest) and
  32-bit BLAKE2s (≤32-byte digest), each with plain, variable-length, and
  keyed (MAC) modes plus streaming ctxs: `blake2{b,s}_{hex,bytes}`,
  `*_{hex,bytes}_n` (variable length), `*_keyed_{hex,bytes}`. Validated
  against RFC 7693 reference vectors (plain + keyed).

### Changed

- **`make contrib` / `install-contrib` now build and ship two more host
  bridges** — `contrib.host.factor` (dlopen libfactor; archive builds bare,
  Factor runtime only needed to *run* code) and `contrib.host.aether`
  (Aether-hosts-Aether fork+exec sandbox; libc + in-tree sandbox runtime
  only). Both were already present in the tree but were missing from the
  build catalogue / install set; they now join the other in-process bridges.
  Adds `tests/integration/host_aether/` (compile + run round-trip) alongside
  the existing factor test. `host/{java,go}` remain out of v1 (javac/jar and
  cgo c-archive don't fit the cc→ar pipeline).

### Removed

- **`contrib/climate_http_tests/`** — the Servirtium climate-API record/replay
  harness moved to the servirtium-vcr repo (`integration/climate_interop/`),
  where the VCR tapes + record-then-replay tests live alongside the
  other-language reference implementations. The copy here was a stale,
  byte-identical 2-file subset already excluded from install.

## [0.298.0]

### Added

- **AES-GCM + RSA-OAEP + RSA-PSS** — the remaining mainstream symmetric-AEAD
  and modern-RSA-padding gaps from the Bouncy Castle port (#739), pure Aether,
  no externs to OpenSSL.
  - **`contrib.cryptography.aes`** gains `gcm_seal` / `gcm_open` (AES-GCM,
    NIST SP800-38D): GHASH over GF(2^128), J0/CTR encryption, 16-byte auth tag
    with constant-time compare. Validated against NIST GCM test cases 1/2/4.
  - **`contrib.cryptography.rsa`** gains `encrypt_oaep` / `decrypt_oaep`
    (RSAES-OAEP) and `sign_pss` / `verify_pss` (RSASSA-PSS), RFC 8017 with
    SHA-256 + MGF1. Validated against reference vectors (byte-exact encrypt /
    sign, decrypt / verify, round-trips). `encrypt_oaep` and `sign_pss` take
    the seed / salt as a parameter for determinism (callers pass a CSPRNG
    value in production).
- **`tests/integration/c_import_struct_no_typedef`** — regression guard for
  `aetherc` emitting `struct Name *` (not bare `Name *`) for pointers to
  `@c_import` structs that ship no convenience typedef (the `struct tm` /
  `struct stat` shape). The fix landed earlier via #534; this adds the test
  that guards it.

### Changed

- **Heredocs strip common leading-whitespace indent** (`<<MARKER … MARKER`).
  The longest leading-whitespace prefix shared by every non-blank line is now
  removed, so a heredoc body can be indented to match its surrounding code
  without that indentation leaking into the string. Blank lines don't
  constrain the prefix; relative indentation within the block is preserved.
  The match is character-exact — a space-vs-tab disagreement at a column stops
  the strip there (no shifting past a column where lines differ); to keep a
  literal common indent, indent one line less than the rest. The closing
  marker must be at column 0. Docs in `docs/language-reference.md`; regression
  in `tests/regression/test_heredoc_dedent.ae`.

### Fixed

- **Parser: `call(...) | EXPR` is no longer misread as a trailing closure.**
  A `|` (or `||`) immediately after a function call was unconditionally parsed
  as the start of a trailing-closure parameter list (`func(args) |x| { … }`),
  so a bitwise/logical-OR on a call result — `strlen(s) | 0x80` — failed with
  "Expected IDENTIFIER, got NUMBER". A `|`/`||` is now treated as a trailing
  closure only when the parameter list is followed by `{` or `->`; otherwise it
  is left for the expression parser. Genuine typed-param trailing closures
  (`each(xs) |x: int| { … }`, `map(xs) |x: int| -> x*2`) still parse. (Bit the
  AES/ChaCha and Ed25519 crypto ports.) Regression in
  `tests/regression/test_pipe_after_call.ae`.

## [0.296.0]

### Added

- **Elliptic-curve cryptography — X25519, Ed25519, and NIST P-256** in pure
  Aether on top of std.bignum (the largest single bc-csharp / #739 gap). No
  externs to OpenSSL; each validated against its RFC / NIST test vectors.
  - **`contrib.cryptography.x25519`** — X25519 ECDH (RFC 7748): Montgomery
    ladder over GF(2^255-19). `x25519(scalar, u)`, `base_mult(scalar)`.
    Validated against RFC 7748 §5.2 and the §6.1 Diffie-Hellman round.
  - **`contrib.cryptography.ed25519`** — Ed25519 signatures (RFC 8032):
    twisted-Edwards point ops in extended coordinates, SHA-512-based key/nonce
    derivation, point compression/decompression. `publickey`, `sign`, `verify`.
    Validated against RFC 8032 §7.1 Tests 1-3 (exact signatures + verify).
  - **`contrib.cryptography.p256`** — NIST P-256 / secp256r1: short-Weierstrass
    Jacobian point arithmetic, ECDH, and ECDSA. `scalar_mult[_base]`, `ecdh`,
    `ecdsa_sign`, `ecdsa_verify`. Validated against a NIST ECDH CAVP vector,
    the published 2G doubling, and an ECDSA sign/verify round-trip.
  - These are correctness-first ports over the variable-time std.bignum — not
    constant-time/side-channel-hardened; documented in each module header.

## [0.295.0]

### Added

- **Key derivation, AES MAC/wrap, and a ChaCha20-Poly1305 AEAD** — a large
  pure-Aether tranche of the Bouncy Castle port (#739), all validated against
  RFC test vectors, no externs to OpenSSL.
  - **`std.cryptography.hkdf`** — HKDF (RFC 5869) extract/expand over the
    existing HMAC. Validated against RFC 5869 Test Case 1.
  - **`std.cryptography.pbkdf2`** — PBKDF2 (PKCS#5 v2 / RFC 8018) over HMAC.
    Validated against published PBKDF2-HMAC-SHA256 vectors.
  - **`contrib.cryptography.aes`** gains `cmac` (AES-CMAC, RFC 4493) and
    `key_wrap` / `key_unwrap` (AES Key Wrap, RFC 3394) on the existing block
    primitive. Validated against all four RFC 4493 CMAC vectors and the
    RFC 3394 wrap vector (+ unwrap integrity check).
  - **`contrib.cryptography.chacha20poly1305`** — ChaCha20, Poly1305, and the
    ChaCha20-Poly1305 AEAD (RFC 8439): `chacha20_xor`, `poly1305_mac`,
    `aead_seal`, `aead_open` (constant-time tag compare). The pure-Aether
    counterpart to AES-GCM. Validated against the RFC 8439 §2.5.2 and §2.8.2
    vectors (seal reproduces the exact spec ciphertext+tag; tampered tags are
    rejected).

### Fixed

- **Actors: a string message field retained into actor state no longer
  corrupts.** `SetN(in_n) -> { n = in_n }` stored a raw pointer into the
  message envelope's string, which is freed right after the handler returns,
  so a later message read freed bytes. The retain now copies the borrowed
  string into an owned AetherString (freeing any prior copy). Also fixes the
  defensive-copy workaround `n = string.concat(in_n, "")`, which previously
  failed to compile (`'_heap_n' undeclared`) because actor handlers skipped
  the function-scope heap-string hoist pass. (Reported by aeo.)

## [0.294.0]

### Added

- **AES CBC mode + PKCS#7 padding** in `contrib.cryptography.aes`, layered
  over the existing FIPS-197 block primitive (no externs to OpenSSL).
  - `cbc_encrypt` / `cbc_decrypt` — block-aligned CBC (C_i = E(P_i XOR C_{i-1})).
  - `pkcs7_pad` / `pkcs7_unpad` — RFC 5652 §6.3 padding (a full extra block is
    added when the input is already a multiple of 16; `pkcs7_unpad` returns
    `(plaintext, err)` and rejects malformed padding).
  - `cbc_encrypt_pkcs7` / `cbc_decrypt_pkcs7` — arbitrary-length CBC.
  - Validated against NIST SP800-38A F.2.1/F.2.2 CBC vectors plus PKCS#7
    round-trip and bad-padding-rejection cases.
- **RIPEMD-256 and RIPEMD-320 digests** in `std.cryptography`, ported in pure
  Aether from Bouncy Castle's `RipeMD256Digest` / `RipeMD320Digest`. These run
  the two RIPEMD-128 / -160 lines side by side for a wider (256-/320-bit)
  output at the same security level as -128 / -160. Same streaming + one-shot
  surface as the other digests; validated against the published RIPEMD
  reference vectors.
  - **`std.cryptography.ripemd256`** — 256-bit, 8-word dual-line.
  - **`std.cryptography.ripemd320`** — 320-bit, 10-word dual-line.

## [0.293.0]

### Added

- **Three more digests in `std.cryptography`** — RIPEMD-160, RIPEMD-128, and
  SM3, each ported in pure Aether from Bouncy Castle's
  `RipeMD160Digest` / `RipeMD128Digest` / `SM3Digest` (no externs to OpenSSL
  or any C crypto). Each module exposes one-shot `*_hex` / `*_bytes` and a
  streaming `new` / `update` / `update_bytes` / `final_hex` / `final_bytes` /
  `free_ctx` surface, mirroring `std.cryptography.sha2`.
  - **`std.cryptography.ripemd160`** — 160-bit RIPEMD (the second hash in
    Bitcoin's HASH160). Little-endian dual-line 80-round compression.
  - **`std.cryptography.ripemd128`** — 128-bit RIPEMD; 4-word, 64-round
    dual-line variant.
  - **`std.cryptography.sm3`** — Chinese SM3 (GB/T 32905); 256-bit,
    SHA-256-like big-endian construction.
  - All three validated against published test vectors (empty / `abc` /
    `message digest` / alphabet / multi-block inputs) with streaming-vs-one-shot
    consistency checks; see `tests/integration/crypto_{ripemd160,ripemd128,sm3}`.

## [0.292.0]

### Added

- **FreeBSD sandbox parity + Capsicum / Casper / audit** (`std.capsicum`,
  `std.casper`, `std.audit`) — revives `feat/freebsd-sandbox-parity` onto
  current main. Pure Aether + `#if`-guarded C; degrades cleanly off FreeBSD.
  - **`std.capsicum`** — FreeBSD Capsicum bindings: `available` / `enter` /
    `in_mode` / `rights_limit` / `fcntls_limit` with the full `R_*` / `F_*`
    constant set. `available()` returns 0 off FreeBSD (or on a kernel without
    Capsicum) and `enter()` returns `CAP_UNSUPPORTED` (-2) — portable code
    branches on `available()` before relying on enforcement, never crashes.
    Phase-2 self-sandbox at startup (`runtime/sandbox/capsicum_autosandbox.c`).
  - **`std.casper`** — Casper service delegation (DNS / passwd / sysctl) across
    the capability-mode boundary, with the mandatory two-phase ordering baked
    into the docstring (open service channels *before* `capsicum.enter()`).
    libcasper + per-service libs are resolved by globbing the actual `.so.*`
    filenames (GhostBSD lacks the `.so` linker symlinks); empty → stub path.
  - **`std.audit`** — audit trail for the in-process permission layer
    (`runtime/sandbox/aether_audit.{c,h}`).
  - Runtime sandbox split into `runtime/sandbox/spawn_sandboxed_{bsd,linux,stub}.c`
    (`#if defined(__FreeBSD__)/__linux__`-guarded; the Linux impl moved from
    the old single `runtime/aether_spawn_sandboxed.c`). The LD_PRELOAD
    containment shim now also builds on FreeBSD.
  - Examples: `capsicum-demo.ae`, `casper-demo.ae`, `audit-demo.ae`.

  Ported by an author who got Capsicum right (the two-phase Casper ordering).
  Downstream consumer: the **aeo** orchestrator's host-adaptation / fast-fail
  grammar (`require_capsicum()` / `prefer_capsicum()`) sits directly on this
  surface. **Deferred follow-ups:** automatic Capsicum wiring into
  `spawn_sandboxed` (consumers call `capsicum.enter()` explicitly for now), and
  exposing fds from `std.file` / `std.net` handles so `rights_limit()` works on
  more than raw/inherited descriptors.

## [0.291.0]

### Added

- **`contrib.cryptography.aes` — AES (FIPS-197)** (issue #739) — the
  block-cipher core that unblocks the entire symmetric surface (CBC / CTR /
  CFB / OFB / ECB / GCM / CCM / EAX / OCB / AES-key-wrap / AES-CMAC /
  AES-CTR-DRBG all drive exactly this primitive). Pure Aether, byte-oriented
  FIPS-197 reference form (256-byte S-box + inverse, GF(2⁸) `xtime`) — chosen
  over Bouncy Castle's T-box `AesEngine` for auditability and to avoid the
  cache-timing surface of big T-tables (a T-box/AES-NI fast path is a later
  perf slice). 128/192/256-bit keys. Surface: `new_encryptor` / `new_decryptor`
  / `process_block` (the 16-byte primitive the modes call), plus `ecb_encrypt`
  / `ecb_decrypt` (block-aligned, no padding) and `ctr_xor` (CTR stream).
  Verified against the FIPS-197 Appendix B/C known-answer vectors for all three
  key sizes and the NIST SP800-38A F.5 AES-128-CTR vector (also reproduced via
  `openssl enc -aes-128-ctr`); ASan-clean. Regression:
  `tests/regression/test_aes.ae`. No OpenSSL AES — the round functions, key
  schedule, and modes are all Aether. Padded modes (CBC/PKCS#7), the AEADs
  (GCM/CCM/ChaCha20-Poly1305), and key-wrap are follow-up slices on this core.

## [0.290.0]

### Added

- **`contrib.cryptography.pem` / `.asn1` / `.rsa`** (issue #739) — the format
  layer that turns `std.bignum` into usable RSA, all pure Aether (no OpenSSL
  RSA; the OS CSPRNG via `std.cryptography.random_bytes` is the only extern,
  for PKCS#1 v1.5 padding randomness).
  - **`contrib.cryptography.pem`** — RFC 7468 PEM `parse` / `encode` over a
    self-contained RFC 4648 base64 codec (no extern base64). 64-column line
    wrapping, BEGIN/END label-match validation.
  - **`contrib.cryptography.asn1`** — ASN.1 **DER** parser + emitter over
    `std.bytes`: TLV read with `last_tag`, typed readers/encoders for INTEGER
    (via `std.bignum`), SEQUENCE, OBJECT IDENTIFIER, OCTET/BIT STRING, NULL,
    BOOLEAN. Ported from Bouncy Castle's `asn1/`.
  - **`contrib.cryptography.rsa`** — the first `std.bignum` consumer: key from
    components or PKCS#1 `RSAPrivateKey` DER, raw `m^e`/`c^d mod n` via
    `bignum.mod_pow`, and PKCS#1 v1.5 encrypt/decrypt + sign/verify (over a
    caller-supplied DigestInfo, so RSA stays hash-agnostic). Ported from
    Bouncy Castle's RSA engine + `Pkcs1Encoding` + `RsaDigestSigner`.

  Cross-validated against OpenSSL end-to-end on a real RSA key: our code
  decrypts an OpenSSL PKCS#1 ciphertext, verifies an OpenSSL SHA-256
  signature, and **our v1.5 signature is byte-identical to OpenSSL's**; the
  ASN.1 codec reproduces a real key's DER byte-for-byte on re-encode.
  Regressions: `tests/regression/test_{pem_codec,asn1_der,rsa_pkcs1}.ae`.
  Constant-time decryption, OAEP, PSS, and X.509/PKCS#8 are follow-up slices.

## [0.289.0]

### Added

- **`std.bignum` — `mod_pow` / `gcd` / `mod_inverse` / `is_probable_prime`**
  (issue #739, the layer that completes the BigInteger surface for RSA/DSA).
  `mod_pow` is square-and-multiply modular exponentiation; `gcd` is Euclid;
  `mod_inverse` is iterative extended Euclid (returns `null` when no inverse
  exists, i.e. `gcd(a,m) != 1`); `is_probable_prime(n, rounds)` is Miller-Rabin
  over a fixed set of small witness bases (deterministic for all n < 3.3e24,
  a strong probable-prime test beyond). Bouncy Castle uses Barrett/Montgomery
  reduction for `ModPow` and Montgomery-form Miller-Rabin; the textbook forms
  here give identical results with no extern crypto — the Montgomery fast paths
  are a tracked follow-up optimization. Fuzzed against Python over 366
  `mod_pow`/`gcd`/`mod_inverse` cases (operands up to 128 bits) plus a primality
  sweep over 2..2000 and several 32-bit knowns (incl. the Carmichael number
  561 and the Mersenne prime M31); ASan-clean across a heavy mixed-op loop.
  Regression: `tests/regression/test_bignum_modpow.ae`. This completes the
  arbitrary-precision integer surface (#739 Tier-2 gate) that unblocks
  RSA/DSA/ECDSA/X.509. Still pure Aether — no externs to OpenSSL or any C
  bignum library.

## [0.287.0]

### Added

- **`std.bignum` — multiply / divide / remainder / mod** (issue #739, the
  layer after the foundation). `multiply` is Bouncy Castle's schoolbook
  `Multiply(uint[],uint[],uint[])`; `divide` / `remainder` / `mod` are binary
  long division over the unsigned magnitudes with BC's sign rules (quotient
  truncates toward zero with sign `a.sign*b.sign`; remainder takes the
  dividend's sign; `mod` is always in `[0,|b|)`). The whole surface was fuzzed
  against Python's arbitrary-precision `int` over 414 signed multi-limb cases
  (including the 5-limb / 2-limb division a first shift-division port looped
  on) and is ASan-clean across the intermediate-heavy divmod loop. Regression:
  `tests/regression/test_bignum_muldiv.ae`. Still pure Aether — no externs to
  OpenSSL or any C bignum library. **`mod_pow` / `gcd` / `mod_inverse` /
  `is_probable_prime` remain follow-up layers**; `mod_pow` (the RSA workhorse)
  will use Montgomery reduction rather than this O(n²) division.
- **`std.bignum` — arbitrary-precision integers (foundation layer)** (issue
  #739 slice 11, the BigInteger watershed). Pure Aether, ported from Bouncy
  Castle's `BigInteger.cs`: sign-magnitude representation (32-bit limbs over
  `std.intarr`, big-endian, separate sign in {-1,0,1}). This first layer
  covers `from_bytes` / `to_bytes` (two's-complement **signed**) +
  `from_bytes_unsigned` / `to_bytes_unsigned` (magnitude) over `std.bytes`
  (consistent with the rest of the cryptography port), `from_int`, `compare`,
  `is_zero`, `sign`, `bit_length`, `add`, `subtract`, `negate`, `abs`,
  `shift_left`, `shift_right`. Every operation was cross-checked against
  Python's arbitrary-precision `int` (add/sub/compare/shift over signed
  integers including INT_MIN; signed+unsigned byte round-trips including the
  `80` / `0080` / `00ff` / -128 two's-complement edges). Regression:
  `tests/regression/test_bignum_foundation.ae`. No externs to OpenSSL or any C
  bignum library. **Multiply, divide/mod, mod_pow, gcd, mod_inverse, and
  is_probable_prime are deferred to follow-up layers** — this foundation is the
  Tier-2 gate that, once complete, unblocks RSA/DSA/ECDSA/X.509.

## [0.286.0]

### Added

- **Native HMAC + HMAC-DRBG** (`std.cryptography.hmac`,
  `std.cryptography.drbg`, issue #739) — pure Aether on the native SHA-2.
  - **`std.cryptography.hmac`** — RFC 2104 HMAC over any native SHA-2 digest,
    working entirely in `bytes` buffers (so it's binary-safe for arbitrary
    keys/messages, including the key-longer-than-block hashed-key path).
    One-shot (`hmac_sha256` / `_hex`, `hmac_sha512`, generic `hmac_bytes` /
    `hmac_hex`) and streaming (`new(algo, key, key_len)` → `update` →
    `final_bytes` / `final_hex`). Verified against `openssl dgst -mac HMAC`
    on RFC 4231 vectors.
  - **`std.cryptography.drbg`** — SP800-90A HMAC-DRBG, ported from Bouncy
    Castle's `HMacSP800Drbg.cs`. Deterministic (caller supplies entropy):
    `new(algo, entropy, …, nonce, …, perso, …)` → `generate` /
    `generate_with_input` / `reseed`. Verified against Bouncy Castle's own
    `HMacDrbgTest.cs` SHA-256 vector (two generates match byte-for-byte).
  - Also adds `sha2.update_bytes(ctx, bytes, len)` — a binary-safe streaming
    update the HMAC construction needs.

  No externs to OpenSSL or any C crypto library. Regression:
  `tests/regression/test_hmac_drbg_native.ae`. Bouncy Castle (MIT) attribution
  on the DRBG; HMAC is the generic RFC 2104 construction. CTR-DRBG is deferred
  until native AES lands.

## [0.285.0]

### Added

- **Native SHA-2 family** (`std.cryptography.sha2`, issue #739 slice 2) —
  SHA-224, SHA-256, SHA-384, SHA-512, SHA-512/224, SHA-512/256, implemented in
  **pure Aether** on the Tier-0 foundations (`std.bits` for logical
  shifts/rotates, `std.longarr` for the 64-bit message schedule, `std.bytes`
  big-endian accessors). No externs to OpenSSL or any C crypto library — the
  compression functions, padding, and length encoding are all Aether code.
  Ported from Bouncy Castle's `GeneralDigest` / `LongDigest` /
  `Sha{224,256,384,512}Digest`. Both one-shot (`sha256_hex` / `sha256_bytes`,
  …) and streaming (`new(algo)` → `update` → `final_hex` / `final_bytes`,
  ctx self-freed on finalize). Every digest was cross-checked against
  `openssl dgst` across all block-boundary input lengths (55/56/63/64/65/
  119/120/127/128). Regression: `tests/regression/test_sha2_native.ae` (NIST/
  RFC vectors + streaming-equals-one-shot). Bouncy Castle (MIT) attribution
  per file. This unblocks native streaming-digest + DRBG (Tier 1 items 5/6),
  which the existing OpenSSL-backed digest ctx will be retired in favour of.

## [0.284.0]

### Added

- **Cryptography port Tier 0 foundations** (issue #739, slice 1) — four
  Aether-native stdlib modules every digest/cipher port depends on:
  - **`std.longarr`** — fixed-size 64-bit packed array, the `long`-cell twin
    of `std.intarr` (SHA-512 / Keccak / GCM / Poly1305 / lattice-PQC state).
  - **`std.bits`** — unsigned-bit helpers Aether's signed `int`/`long` can't
    express directly: `lsr32/64` (logical right shift — Aether's `>>` is
    arithmetic), `rotr/rotl 32/64`, `popcount32/64`, `clz32/64`, `udiv/urem
    32/64`, `ucmp64`. Ported from Bouncy Castle's `Integers.cs` / `Longs.cs`.
  - **`std.bytes` big-endian accessors** — `set_be16/32/64` + `get_be16/32/64`,
    the BE twin of the existing `_le*` family (cryptography wire format is mostly
    big-endian). Modelled on Bouncy Castle's `Pack.cs`.
  - **`std.bytes.cursor`** — forward read-position over a bytes buffer
    (`read_u8`, `read_be_u16/32/64`, `read_slice`, `remaining`, `peek`, `eof`,
    `pos`/`seek`); foundation for byte parsers (ASN.1, PEM, OpenPGP).

  Regression tests (`tests/regression/test_{bits,longarr,bytes_be,bytes_cursor}.ae`)
  include vectors ported from Bouncy Castle's `IntegersTest`/`LongsTest`.
  Bouncy Castle (MIT) attribution per ported file plus a new top-level
  `THIRD_PARTY_LICENSES.md`.

## [0.283.0]

### Fixed

- **Module token cap raised 20000 → 100000, buffer heap-allocated**
  (`compiler/aether_module.c`). `module_parse_file` capped imported modules at
  `MAX_MODULE_TOKENS` tokens; a larger module was silently truncated
  mid-token-stream, dropping its tail declarations so callers hit spurious
  `E0301: Undefined function` on the missing symbols. Raised the cap 5× and
  moved the token buffer off the stack to a `malloc`'d array (a fixed
  100k-entry stack array would risk overflow), with NULL-check cleanup and a
  matching free. Regression: `tests/integration/module_token_cap` imports a
  ~2200-function module (>20k tokens) and calls its first, middle, and last
  function — truncation under the old cap left the tail undefined.

## [0.282.0]

### Fixed

- **`fn name(...)` is now a first-class top-level function definition**
  (#791). `fn` is not a reserved word (it doubles as the function-pointer
  type head `fn(...) -> R`), so a top-level `fn name()` previously only
  survived via parse-error recovery: the parser raised "unexpected
  identifier at top level" on `fn`, recovery skipped the token, and
  `name(...)` then parsed as a function. That recovery is silent when a
  module is imported but fatal on a standalone / strict re-parse, so at
  full module-graph scale a re-parsed sibling module that used the `fn`
  spelling (e.g. std.uuid, std.url) surfaced the recovery as a spurious
  top-level parse error attributed to that module. The top-level parser
  now recognises `fn` + name + `(` as a function definition directly, so
  the spelling is first-class and parses identically on every path
  (standalone build, import, and re-parse). `fn`-typed parameters
  (`f: fn(int) -> int`) still parse as types — the definition form is
  distinguished by the identifier between `fn` and `(`.

## [0.281.0]

### Fixed

- **`std.fs`: export `join_clean` and `first_element`.** The two
  path-cleaning wrappers added in `std.fs: add join_clean + first_element`
  were defined but omitted from the module `exports (...)` list, so
  `fs.join_clean(...)` / `fs.first_element(...)` failed at the call site
  with `E0301: Undefined function` (the `tests/integration/fs_join_clean`
  regression went red). Added both to the export list.

## [0.280.0]

### Changed

- **VS Code extension: grammar, snippets, and ergonomics refresh**
  (`editor/vscode/`). Refreshed the TextMate grammar for missing keywords,
  types, and `@annotations`; added range/spread operators, the `make` keyword,
  and snippets; block-comment on-enter + indent rules; a cross-platform
  "Erlang-style" palette; and README palette/install-snippet docs. Also
  rebuilt `out/extension.js` with an activation-leak fix. Editor tooling only —
  no compiler, std, or runtime change.

## [0.279.0]

### Added

- **`heap.new(T)` / `heap.free(p)` — POD struct heap allocation** (issue
  #564; `compiler/parser`, `compiler/analysis`, `compiler/codegen`).
  `ctx = heap.new(AppCtx)` allocates a zero-initialised `AppCtx` on the
  heap and returns `*AppCtx`; fields are read/written through the pointer
  (`ctx.port = 8080`) and the box is reclaimed with `heap.free(ctx)`
  (NULL-safe). Lowers to `((T*)calloc(1, sizeof(T)))` / `free(p)` — the
  guaranteed zero-init the memory-safety review requires. **POD-only**: the
  type must be a struct with no `string` (or other heap-managed) field; a
  non-POD struct is a compile error directing the author to hold heavy data
  as an opaque handle the struct doesn't own. This is the safe first cut
  from the issue's recommendation #1 — richer boxes that own their fields
  need an ownership model (retain-on-store + typed free) specced first.
  Replaces the `malloc(64) as *AppCtx` magic-number pattern with a
  type-safe, self-documenting primitive. Pairs with the
  `defer heap.free(p)` idiom for scope-bounded lifetimes.

## [0.278.0]

### Added

- **`expr!` unwrap-or-trap operator** (`compiler/parser`, `compiler/analysis`,
  `compiler/codegen`). A postfix `!` on a `(value, err)` tuple yields the
  first slot and panics if the trailing (string) error slot is non-empty:
  `h = cryptography.random_hex(n)!` replaces the two-line
  `h, e = ...  return h` discard wrapper. Works on any tuple whose final
  slot is the `string` error (2-tuples, the `(bytes, len, err)` 3-tuple,
  …); the result type is the first slot. Composes anywhere an expression
  is allowed — assignment RHS, call arguments — via a GCC
  statement-expression that evaluates the tuple once. `!` stays the actor
  fire-and-forget operator when followed by a message type (an
  uppercase-leading identifier); the unwrap reading applies everywhere
  else. A non-tuple or string-less-final-slot operand is a compile error.

### Fixed

- **`import std.fs (*)` (glob import) now carries the real tuple return
  types of `(value, err)` wrappers** (`compiler/analysis/typechecker.c`).
  A glob import registered each short alias by cloning the full symbol's
  type *before* return-type inference ran, so a wrapper whose return type
  is inferred (e.g. `fs.list_dir`'s `(ptr, string)` tuple) left the bare
  alias `list_dir` stuck on a pre-inference `int` placeholder. A
  `list, err = list_dir(...)` then stamped the call's return type as
  `int` and codegen emitted `int _tup0 = fs_list_dir(...)` — a C type
  error. Namespaced (`fs.list_dir`) and selective imports already worked;
  the glob form did not. Import-alias short symbols are now re-synced from
  their inferred full symbols after type inference, so all three import
  forms agree. (fbs-core ask #1.)

### Added

- **Streaming (incremental) digest context in `std.cryptography`**
  (`std/cryptography/`). `digest_new(algo)` returns an opaque context;
  `digest_update(ctx, data, n)` feeds bytes in pieces; `digest_final_hex(ctx)`
  / `digest_final_bytes(ctx)` finalize (and free the context). `algo` uses
  the same names as `hash_hex` ("md5", "sha256", "sha1", "md4", ...).
  This hashes data that arrives in windows without ever holding it whole —
  a blob store can now compute an upload's ETag as it streams to disk
  instead of reading the stored object back purely to MD5 it (S3 ETag =
  md5-of-object; multipart ETag = md5-of-md5s). `digest_free(ctx)` is the
  abandon-without-finalize cleanup path. Thin veneer over libcrypto's
  `EVP_DigestInit/Update/Final`; returns the "openssl unavailable" error
  shape on builds without OpenSSL.
- **`fs.join_clean(a, b)` and `fs.first_element(path)`** (`std/fs/module.ae`).
  `join_clean` is `path_join` followed by `clean` in one call — the
  cleaned path that actually reaches the filesystem after a caller-
  supplied segment is appended, so `fs.join_clean("bucket", "a/../b")`
  collapses to `bucket/b` rather than leaving the traversal in place
  (path-traversal-defense invariant for object stores). Empty-segment
  handling mirrors `path_join`'s identity behaviour. `first_element`
  returns the leading cleaned path component (`fs.first_element("/a/b")`
  → `"a"`). Together they let downstream blob-store code drop its
  hand-rolled `pathutil.join` wrapper.

## [0.277.0]

### Fixed

- **contrib host bridges: lua + tcl compile against newer Homebrew /
  Tcl-9 libraries** (`contrib/host/lua/aether_host_lua.c`,
  `contrib/host/tcl/aether_host_tcl.c`). Both dlopen bridges name C-API
  functions as struct fields and call them as `g_lib.Fn(...)`, which the
  preprocessor mangles when the library header turned `Fn` into a
  function-like macro:
  - **Lua 5.4** (Homebrew): `luaL_openlibs(L)` is
    `#define`d to `luaL_openselectedlibs(L, ~0, 0)`, so
    `g_lua.luaL_openlibs(L)` rewrote to a non-existent
    `luaL_openselectedlibs` member (macOS build break; Debian's Lua 5.4
    ships it as a real declaration, which is why Linux CI never saw it).
    Fix: `#undef luaL_openlibs` after the headers and dlsym the real
    exported symbol (present in every shipping liblua).
  - **Tcl 9.0** (Homebrew): `Tcl_GetStringResult` and `Tcl_GetString`
    became function-like macros over `Tcl_GetStringFromObj` and are no
    longer exported, so the struct-field calls referenced non-existent
    members. Fix: `#undef` both, resolve the lowest-common-denominator
    real exports `Tcl_GetStringFromObj` + `Tcl_GetObjResult` (present in
    both 8.6 and 9.0), and recompose the two accessors as local helpers.
  Both verified by compiling each bridge against the real (8.6 / 5.3 /
  5.4) headers and against simulated Homebrew-macro headers — clean with
  the fix, reproduces the reported break without it. No behavioural
  change on platforms that were already building (the `#undef`s are
  no-ops where the macro is absent). Surfaced on a macOS/Homebrew
  `make install-contrib` (Lua 5.4.x, Tcl 9.0.3).

## [0.275.0]

### Added

- **`@packed` extern structs** (#747 item 1, the Redis sds.c blocker). An
  `extern struct ... @packed { ... }` emits the C body with
  `__attribute__((packed))`, so the layout has no inter-field padding or
  trailing alignment — the `sdshdr8/16/32/64` shape where the length/
  alloc/flags header sits at fixed packed offsets before the string data.
  `sizeof(S)` / `offsetof(S, f)` lower to C and report the packed numbers,
  and a `*S` overlay reads/writes fields at their packed offsets (verified
  by round-trip). `@packed` is mutually exclusive with `@c_import` (a
  header-defined struct's packing is the header's job; combining them is a
  parse error). Bit-width fields and a trailing flexible array still work
  under `@packed`. Note: pure `@c_import` overlays already inherit the
  header's packed layout (no body emitted), so `@packed` is the tool when
  Aether owns the struct body (a pure-Aether port with no C header). See
  [docs/c-interop.md](docs/c-interop.md) (Packed structs).

## [0.276.0]

### Added

- **Function-pointer struct fields** (#749 Ask A). A struct field typed
  `fn(T1, T2) -> R` now emits the C function-pointer member
  `R (*name)(T1, T2)` (instead of an untyped `void*`), and a call through
  it is a real indirect call: `o.field(args)` for a value struct,
  `p.field(args)` → `p->field(args)` for a pointer-to-struct. This is the
  `dictType` vtable shape that gates the Redis dict.c port (2340 lines)
  and the keyspace command tier. Field PARSING already worked (parse_type
  yields the fn-ptr type); the change is codegen + dispatch: a typed
  field-declarator emitter, plus — because the parser collapses a
  member-access callee to the dotted name `recv.field` and drops the
  receiver — a typecheck branch that recognises `recv.fnptrfield(args)`
  (resolving the field signature off the struct definition, tagging the
  receiver as value vs pointer) and a matching codegen branch that emits
  the indirect call. Single-level receiver (a bare local); the field
  already carries a real C fn-ptr type, so the call needs no cast. Sibling
  of fn-pointer parameters (#750) and typed fn-ptr locals. See
  [docs/language-reference.md](docs/language-reference.md) (Function-
  pointer struct fields).
## [0.274.0]

### Added

- **`longdouble` primitive type** (#749 Ask C, completing the aedis
  core-floor umbrella). Maps to C `long double` — the widest floating
  type — for the exact-decimal numeric paths a C interop layer needs
  (libc `strtold`, INCRBYFLOAT / sorted-set score conversion,
  object.c/util.c number formatting). Supports arithmetic (`+ - * /`),
  comparison, and conversion to/from `int` and `float`; as the widest
  numeric it wins promotion (`longdouble op int` / `op float` →
  `longdouble`). Usable in locals, params/returns, struct fields, and
  `extern` signatures; formatted with `%Lg`/`%Lf` in interpolation and
  `print`. No source literal — values arrive via an extern or by widening
  an `int`/`float`. Spelled as the type name `longdouble` (no new keyword
  token). See [docs/language-reference.md](docs/language-reference.md)
  (`longdouble`).

  With this, the #749 umbrella is fully addressed: fn-pointer parameters
  (#773) and struct fields (#777) for Ask A, the inline `...` C-varargs
  call-through already shipped for Ask B, and `longdouble` for Ask C.

## [0.273.0]

### Added

- **By-value struct returns and stack-struct locals** (#746). A function
  may now declare a by-value struct return type (`make() -> Pair`), and a
  struct can be declared as a stack-allocated local (`Pair p` — no `*`, no
  initializer) and filled field-by-field. Both were parse errors before:
  the `-> StructName` return type fell through the `->` return-type
  disambiguator (an off-by-one in its `{` lookahead — `->` is already
  consumed, so the name sits at offset 0, not 1) into the `-> expr`
  arrow-body path; and `StructName name` had no statement-level
  declaration case (only `*StructName name` and the C-ABI aliases like
  `size_t n`). Both fixes are parser-only — the `IDENT IDENT` stack-local
  case mirrors the existing `*StructName name` pointer path, and codegen
  was already correct (struct return type via get_c_type, `.field` access
  on a value struct, `return p` as a C struct copy). Completes the
  by-value struct set (by-value params already worked), so an all-scalar
  record (a geometry/bounding-box result, the geohash_helper.c shape) can
  be built on the stack and returned without heap allocation or an
  out-pointer. See [docs/language-reference.md](docs/language-reference.md)
  (By-value struct returns and stack-struct locals).
## [0.272.0]

### Added

- **Function-pointer parameters** (#750). A `fn(T1, T2) -> R` parameter now
  lowers to the exact C function-pointer type `R (*name)(T1, T2)` in both
  the prototype and the definition, and a call through it (`cb(a, b)`) is a
  real typed indirect call. Previously a fn-typed parameter collapsed to a
  bare `void*` and the body call emitted invalid C ("called object is not a
  function"); the `as fn(...)` cast only rescued a single in-body callback,
  which didn't scale to multiple callback params or callback-taking helpers.
  This is the parameter form of the existing typed-fn-pointer machinery
  (`as fn(...)` locals, fn-pointer struct fields): the parser/typechecker
  already carried `is_fnptr` onto the parameter, so the fix is codegen-only —
  a fn-ptr declarator emitter for the prototype + definition, plus
  registering the param in the fn-ptr-local registry so the call site emits
  the typed indirect call. Unblocks porting callback APIs (Redis dictScan/
  raxWalk/command-table iteration; qsort, signal handlers, libcurl/sqlite
  hooks). See [docs/language-reference.md](docs/language-reference.md)
  (Function-pointer parameters).

## [0.271.0]

### Fixed

- **Parser: terminate expression continuations at newlines**
  (`compiler/parser/parser.c`). A line-leading token was sometimes folded into
  the previous line's expression as a continuation, so statements that begin a
  fresh line (e.g. a following `[...]` or call) could be mis-grouped. The
  parser now ends an expression continuation at a newline, matching the
  line-oriented statement model; net simplification of the continuation logic.
  Covered by `tests/syntax/test_parser_line_leading_statements.ae` and a new
  `test_parser_newline_bracket` regression.

## [0.270.0]

### Fixed

- **Parser: newline now terminates infix/postfix expression continuation**
  (issue #528; `compiler/parser/parser.c`,
  `tests/regression/test_parser_line_leading_statements.ae`,
  `tests/integration/parser_newline_bracket/`). The old guarded
  recogniser handled `*StructName name`, `*ident = ...`, and a narrow
  `[x, y]` shape, but still let line-leading unary statements like `-x`
  fold into the previous expression. The binary-expression loop now
  stops whenever an infix operator starts on a later source line, and
  postfix indexing does the same for newline-led `[`. Multiline
  continuations remain supported by placing the operator before the
  newline (`total = a +` newline `b`).

### Changed

- **Codegen cleanup: removed the now-dead #759 tuple-struct heap-flag
  transfer, superseded by #762's return-escape contract**
  (`compiler/codegen/codegen_stmt.c`). Two independent fixes for #752
  (struct-with-heap-string-field returned via tuple) both landed: #759
  zeroed the source struct's `_heap_<field>` flags before the
  function-exit `<Struct>_destroy` defer, and #762 (later, more complete)
  suppresses that destroy entirely on the escaping struct and pushes the
  destroy to the *receiving* caller. With #762's suppression the destroy
  never runs in the callee, so #759's flag-zeroing became a dead store
  (`r._heap_s = 0;` on a struct whose destructor is gone). Removed the
  `emit_tuple_struct_heap_ownership_transfer` helper and its sole call
  site; #762's `mark_returned_struct_escaped` on the same tuple-return
  loop is the live, complete mechanism. No behavioural change — verified
  the generated C drops the dead store while the caller-side single free
  is unchanged; both #752 regression tests
  (`tests/integration/issue_752_struct_string_tuple/`,
  `tests/regression/test_struct_string_field_return.ae`) and unit 229/229
  stay green. Pure dead-code removal; keeps the two-mechanisms-on-one-path
  hazard from misleading a future editor.

## [0.269.0]

### Added

- **RAM-bounded streaming request bodies (#626 upload half)**
  (`std/net/aether_http_server.c`, `std/net/aether_http_server.h`,
  `std/http/module.ae`, `tests/integration/http_stream_upload/`). The
  HTTP/1.1 server no longer buffers a large request body whole before
  dispatching the handler. When a request's `Content-Length` exceeds one
  connection buffer (16 KiB), the dispatcher parses only the header
  block and hands the handler a *streaming* request; `http.request_body_read(req,
  off, max)` then pulls each window straight off the socket. Peak server
  memory for a large upload is one window per connection (O(buf + chunk))
  instead of O(Content-Length) — for N concurrent M-byte PUTs that's the
  difference between N×M and N×window bytes live. The canonical loop the
  fbs-core ask sketched works unchanged:
  ```aether
  total = http.request_body_length(req)
  off = 0
  while off < total {
      chunk, n, _ = http.request_body_read(req, off, 65536)
      if n == 0 { break }
      fs.pwrite(out, chunk, n, off)   // stream → disk, never whole in RAM
      off = off + n
  }
  ```
  Small bodies keep the legacy fully-buffered path (random-access offsets,
  no behavioural change); streaming reads must be sequential (the socket
  isn't seekable). A post-handler drain consumes any body bytes the
  handler left unread so the keep-alive connection boundary stays clean
  for the next request (verified: a follow-up GET on the same socket
  after a 3 MiB streamed PUT still answers correctly). New
  `HttpRequest` streaming fields are trailing/ABI-stable (same promise as
  the connection-metadata block). Closes the upload half of #626
  (download/sendfile half shipped earlier); sourced from
  `stdlib-streaming-upload-body-followup.md` (fbs-core, which measured the
  buffered-upload peak the streaming path removes). Integration test PUTs
  3 MiB and asserts bounded streaming + byte-identical SHA-256 round-trip
  + clean keep-alive boundary.

## [0.268.0]

### Added

- **Typed module-level constant arrays** (#745). `const NAME: T[N] = [...]`
  declares a file-scope `static const <T> NAME[]` lookup table with the C
  element width pinned — `T` ∈ {`uint8`, `uint16`, `uint32`, `uint64`,
  `int`, `long`}. Previously the only spelling was `const NAME[] = [...]`,
  which always inferred `int` elements: a uint8/uint16 table cost 4× the
  memory and mismatched a C header expecting a packed `uint16_t[]` (e.g.
  the cluster-slot CRC16 table). The table is allocated once and shared
  across calls (not re-initialised per call). Two compiler changes: the
  top-level `const` parser now accepts a `: T[N]` annotation (and a typed
  scalar `const NAME: T = value`), and the short unsigned width names
  `uint8`/`uint16`/`uint32` are recognised type spellings (siblings of the
  existing `uint64` keyword, emitting `uint8_t`/`uint16_t`/`uint32_t`); an
  integer-element array literal may initialise a narrower integer-element
  typed const array (the explicit, compile-time-constant intent). See
  [docs/language-reference.md](docs/language-reference.md) (Module-level
  constant arrays).

## [0.267.0]

### Fixed

- **Heap string fields of a struct returned from a function are no longer
  corrupted** (#752, follow-up to #634). When a function returned a struct
  with a heap-string field (directly via a single-value builder return, or
  as a tuple element `return r, ""`), the struct's `<Struct>_destroy`
  function-exit defer freed the field even though the struct escaped via
  the return — so the caller read a dangling pointer and the string came
  back as garbage. Int fields survived (no free); a literal-initialised
  string survived (static), which is why the #634 test (int-only) missed
  it. Two-sided fix matching the established return-escape contract for
  plain heap strings: (1) the callee suppresses the struct's destroy when
  it escapes via a return (`return_escaped_struct_vars` → consulted by
  `try_emit_struct_destroy`), transferring ownership to the caller; (2) the
  caller that *receives* an owned struct — a tuple-unpack target or a local
  initialised from a struct-returning call — gets a `<Struct>_destroy`
  defer so the fields are freed exactly once at its scope exit. Verified
  leak-free and double-free-free (ASan + `leaks`) across tuple, single, and
  chained receive-then-re-return forms. Regression test
  `tests/regression/test_struct_string_field_return.ae` asserts the string
  field's *value* (a behavioural gate, unlike the compile-only #634 test).
## [0.266.0]

### Fixed

- **Module-global first-assigned inside a nested block no longer shadowed
  by an uninitialized local** (#744, regression in #701). Codegen's
  branch/loop variable hoisters (`hoist_if_branch_vars`,
  `hoist_if_else_common_vars`, `hoist_loop_vars`) pre-declared a `var`
  global as a fresh function local when its first assignment appeared
  inside an `if`/`while` body — shadowing the file-scope `static`, so
  every write landed in the local and the global kept its initializer
  forever. A silent miscompile whose visibility depended on optimization
  (it corrupted the aedis MT19937-64 PRNG port: a lazily-malloc'd state
  buffer's writes never reached the global). All three hoisters now skip
  names that are module globals — the write is already routed to the
  static by the variable-declaration emitter (`is_module_global_var`), so
  the local must not be emitted. Regression test in
  `tests/integration/module_globals/nested_block_init.ae` (if-body,
  loop-body, and a lazily-initialised counter; exits non-zero if a write
  fails to reach the global).
## [0.265.0]

### Fixed

- **#752: heap-string fields of a struct returned via tuple were freed
  before the caller could read them** (`compiler/codegen/codegen_stmt.c`,
  `tests/integration/issue_752_struct_string_tuple/`). A function
  returning `(R, err)` where `R` contains a `string` field initialised
  from a heap source (e.g. `string.copy(...)`) emitted:
  ```c
  _tuple_R_string _builder_ret = (_tuple_R_string){r, ""};
  /* deferred */ R_destroy(&r);   // ← frees r.s
  return _builder_ret;
  ```
  The tuple literal memcpys `r` into the returned tuple including its
  `.s` pointer; the immediately-following `R_destroy(&r)` defer then
  frees that pointer's buffer while the caller's copy still references
  it. Use-after-free; caller saw garbage in every string field while
  scalar fields survived. New helper
  `emit_tuple_struct_heap_ownership_transfer` walks every tuple-return
  child that is a bare `AST_IDENTIFIER` of struct type and emits
  `<varname>._heap_<field> = 0;` for each heap-string field after the
  tuple literal is built and before the defer drain. The struct's
  `_destroy` defer becomes a no-op for the transferred fields; the
  caller's returned struct retains `_heap_<field> = 1` so its own
  destruct path correctly reclaims the buffer. Sourced from fbs-core's
  attempt to convert `object_get` from a positional 8-tuple to
  `(Object, err)` (issue #752 repro). The fix only touches the
  with-defer multi-value-return path — the no-defer path constructs
  the tuple inline in `return (Tuple){...};` and has no destroy defer
  to race against.

### Added

- **`std.json.from_int(n)` — integer-flavoured number constructor**
  (`std/json/aether_json.c`, `std/json/aether_json.h`, `std/json/module.ae`,
  `tests/integration/json_from_int/`). Sibling of `json.num(value: float)`:
  takes a `long` (full int64 range) and stamps a `JV_FLAG_INTEGER` flag on
  the `JsonValue` so the serializer emits `%lld` instead of `%g`. The
  motivating bug: `json.num(53248000.0)` serialised as `"5.3248e+07"`
  (`%g` switches to scientific notation past ~1e7), wrong for byte-count
  / ID / total fields and lossy past 2^53. Adds a dedicated `integer`
  slot to the JsonValue union (shares the slot, no struct growth) and
  branches the encoder + `json_get_int` + `json_get_number` + clone-tree
  paths on the flag. Parser-side automatic flagging (recognising bare
  integers in input JSON) is a separate follow-up — the value still
  round-trips correctly via the float path as long as it fits in 2^53.
  Sourced from `stdlib-json-integer-value-ask.md` (fbs-core /metrics).

## [0.264.0]

### Documentation

- Docs only change to repair CHANGELOG

## [0.263.0]

### Fixed

- **Codegen: fixed-array locals hoisted out of a loop/branch body are
  declared `T name[N]`, not the invalid `T[N] name`** (PR #753,
  `compiler/codegen/codegen_stmt.c`,
  `tests/regression/test_hoist_array_local_in_loop.ae`). A `byte[N]` /
  `T[N]` local declared inside a loop or branch — and not as the block's
  first statement — is pre-declared at function scope by the var-hoisters.
  They emitted `<get_c_type> <name>;`, but `get_c_type(TYPE_ARRAY)`
  returns `T[N]` (valid only in postfix-declarator position), so the
  hoist produced `unsigned char[8] buf;` and the variable came out
  undeclared at its use sites. New `emit_hoisted_local_decl()`
  special-cases `TYPE_ARRAY` to emit `elem name[N];`; both hoist sites
  route through it. The first-statement-in-block decl path was already
  correct. Found via the aedis Redis port's per-loop scratch buffers.

## [0.262.0]

### Changed

- **stdlib caps-audit — `std.net` HTTP client response & request buffers**
  (#461). Routed the two *unbounded* internal buffers in the HTTP client
  (`std/net/aether_http.c`) through the capability allocator: the response-
  body accumulation buffer (`full_response` — the attacker-controlled DoS
  surface: a malicious server can flood the response) and the request-header
  build buffer (`hdr`). Both are self-contained within `http_request_internal`
  with the live size tracked in a local (`cap` / `hdr_cap`), so the
  realloc-delta accounting and the error-/exit-path frees balance exactly;
  the empty-response fallback records its 1-byte size. The request body was
  already capped (prior PR). Bounded, caller-supplied request fields
  (method/url/header structs and strdups) and the redirect/dechunk/header-
  extract helpers are intentionally left on libc — they cross alloc/free
  boundaries into wrapper/test code where caps accounting can't stay
  balanced, and they are not the unbounded surface. Verified: unit 227/227
  (no accounting underflow), `-Werror` clean, http_client_dechunk +
  http_client_redirects integration tests pass (real round-trip through the
  capped response buffer).
## [0.261.0]

### Added

- **`std.cryptography.random_hex(n)` / `random_base64(n)` — printable-secret
  convenience wrappers over `random_bytes`** (`std/cryptography/module.ae`,
  `tests/integration/cryptography_random_hex/`). Two thin Aether-side wrappers
  that draw `n` cryptographically-secure bytes from the OS CSPRNG and return a
  lowercase-hex (2`n` chars) or RFC 4648 §4 unpadded-base64 string respectively.
  Motivating shape: opaque-bearer-token / API-key minting (e.g. SigV4 secret
  keys), where callers want a printable secret and the "obvious random function"
  should resolve to the secure path — not `std.math` (a clock-seeded PRNG fit
  only for sampling). Composes existing primitives; no new C, no new externs.
  Hex emission uses `std.bytes` (O(n) build vs the O(n²) repeated `string.concat`
  path). Sourced from `stdlib-csprng-secure-random-ask.md` (fbs-core), whose
  request items 1 + 2 (`random_bytes` + UUIDv4) already shipped at 0.213.0;
  this lands the convenience wrappers that were the third bullet of the same
  ask. Aether wrappers only (the existing `cryptography_random_bytes_raw` C
  side is unchanged).

- **`long long` type spelling on extern parameters / returns**
  (`compiler/parser/parser.c`, `tests/integration/long_long_extern/`). When
  the parser sees a second `long` after the first, both are consumed and the
  resulting type carries the verbatim C spelling `long long` instead of the
  default `int64_t`. The underlying TypeKind is still `TYPE_INT64`, so all
  arithmetic and typechecking behave identically — only the emitted C
  declaration text changes. Closes the "Minor, real, cheap" item from
  `aedis-core-floor-feature-asks.md`: a libc / POSIX header that spells a
  parameter as `long long` (e.g. `mstime_t` typedef chains, the MT19937 /
  SHA reference impls bundled with Redis) now matches its Aether-side
  prototype byte-for-byte, removing the gcc "conflicting types" error that
  previously forced the generated TU to compile *without* its header.
  Four-case integration test (single arg + return, large-value retention,
  mixed `long long` ↔ `int64_t` round-trip).

## [0.260.0]

### Changed

- **stdlib caps-audit — `std.os` POSIX allocation sites** (#462). Routed the
  unbounded, plugin-influenced heap allocations in `std/os/aether_os.c`
  through the capability allocator (`aether_caps_malloc/realloc/free`) so a
  sandboxed plugin can't inflate them past a memory cap: the command-output
  capture buffers (`os_exec_raw`, `os_run_capture_raw`,
  `os_run_capture_status_raw`, `os_run_pipe_drain_and_wait_raw`,
  `os_run_full_raw`'s stdout/stderr accumulator), the `os_getcwd_raw` path
  buffer, the `os_execv` argv scratch, and the `os_getenv` value. Caller-owned
  returns keep the documented libc-free / fail-safe-upward-drift contract
  (the `io_read_file_raw` / `io_getenv` model); internal/transient buffers
  free through the cap with their exact live size (realloc-failure paths free
  the *old* size). New `caps_os_getenv_denied_past_cap` unit test asserts an
  env read is refused when the cap is below the value size, with the counter
  unperturbed. Bounded sites (the 1-byte empty-heap sentinel, the
  pointer-only argv/envp arrays) and the Windows-specific helpers
  (`utf8_to_wide`/`wide_to_utf8`/`WBuf`/`win_launch`/drain-thread) are left as
  tracked follow-ups; `std/fs/aether_fs.c` remains. Verified: unit 228/228
  (ASan-clean), `leaks(1)` clean on the os example, full `.ae` regression 0
  failures.

---

Older releases (**0.259.0 and earlier**, down to 0.18.0) live in
[CHANGELOG-archive.md](CHANGELOG-archive.md).
