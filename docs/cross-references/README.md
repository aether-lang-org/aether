# Cross-references — comparisons against other languages / projects

Side-by-side feature comparisons between Aether and adjacent
projects, written for the Aether maintainers as decision support:
"what is X doing that we aren't, and is it worth porting?"

Each doc enumerates X's features against Aether's current surface
and tags each one as a candidate to absorb, skip, or treat as
already-covered-differently. The point is **not** to argue Aether
should become any of these — it's to surface attractive ideas
without losing track of where each one came from.

| File | Subject | Notes |
|------|---------|-------|
| [gcp-aether.md](gcp-aether.md) | `GoogleCloudPlatform/Aether` (name-collision repo) | Stalled "vibe-coded demo" with our project's name. Issue #346. |
| [zym.md](zym.md)               | Zym | CLI shell + native bindings. Issue #341. |
| [flint.md](flint.md)           | Flint | LLVM-IR codegen via `lld`. Harder to port from since Aether emits portable C. Issue #339. |
| [fir.md](fir.md)               | Fir | Issue #337. |
| [flux.md](flux.md)             | Flux | Audience: Aether maintainers deliberately choosing what to absorb. Issue #335. |

Each doc was originally drafted in the corresponding GitHub issue
body; lifted into the repo so it stays discoverable next to the
code and survives issue-tracker churn.

For a broader lineage note that is about Aether's own closure/runtime
tradeoff rather than one comparison target, see
[`../closure-lineage-and-runtime-tradeoffs.md`](../closure-lineage-and-runtime-tradeoffs.md).
