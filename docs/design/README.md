# Design & rationale

Why Aether is built the way it is: the reasoning behind specific language and
runtime decisions, comparisons against the alternatives that were considered,
and the concurrency patterns the standard library is designed around. These are
reference material for understanding *why*, not step-by-step guides — for those,
see the [tutorial](../tutorial.md), the [language reference](../language-reference.md),
and the [standard library reference](../stdlib-reference.md).

## Language & runtime rationale

- [Closure lineage and runtime tradeoffs](closure-lineage-and-runtime-tradeoffs.md) —
  why Aether keeps closure-shaped values (`_AeClosure { fn, env }`, `ref()` cells,
  trailing blocks) without adopting a Lisp/Smalltalk runtime, and the tradeoffs
  that choice makes against Lisp, Smalltalk, Rust, Zig, C++, D, and Apple Blocks.
- [Parse, don't validate](parse-dont-validate-review.md) — how `distinct` types
  (#480) and `where`-guards (#525) let the type system carry proof forward instead
  of re-checking it, worked through with compiling examples.
- [Aether through Chlipala's lens](chlipala-lens.md) — a self-assessment of where
  Aether sits on tractability-of-search versus control-over-hardware, and the two
  research directions (static contract discharge, topology-visible actors) it points to.
- [Aether DSL as a rules engine](aether-dsl-as-a-rules-engine.md) — using trailing-block
  DSLs plus `hide` / `seal except` to run business rules as sandboxed `.ae` files.
  The shipped embedding foundation is documented canonically in
  [Embedding & host bindings](../embedded-namespaces-and-host-bindings.md); the
  rules-DSL surface itself is a design exploration.

## Concurrency patterns

- [Sharded actor map](sharded-actor-map.md) — lock striping across `N` owner actors
  to escape the single-owner "one mutex" bottleneck, with FNV-1a routing and
  shard-count guidance.
- [Snapshot cell](snapshot-cell.md) — a lock-free-read / rare-write copy-on-write
  cell (`std.snapshot`) for read-mostly shared state, including the RCU-style
  grace-period reclamation contract required in a non-GC language.
- [Concurrent cache benchmark](concurrent-cache-benchmark.md) — a runnable comparison
  of single-owner, sharded, and snapshot-cell designs, with honest caveats about
  what the single run does and does not measure.
