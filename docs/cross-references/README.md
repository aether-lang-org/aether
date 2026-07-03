# Cross-references — comparisons against adjacent languages and projects

Side-by-side surveys of Aether against nearby language and runtime projects,
written as design history: for each project, what is it doing that Aether isn't,
and is any of it worth adopting? Each survey enumerates the other project's
features against Aether's surface at the time of writing and tags each one as a
candidate to absorb, to skip, or as already-covered-differently.

These are **not** roadmap proposals and **not** an argument that Aether should
become any of these projects. They are a record of what was considered and why,
so an attractive idea and its origin stay discoverable. Ideas that were adopted
are noted inline; anything not yet shipped is a candidate, not a commitment.

| Survey | Subject | Source |
|--------|---------|--------|
| [gcp-aether.md](gcp-aether.md) | `GoogleCloudPlatform/Aether` — a separate, stalled project that shares this project's name | [GoogleCloudPlatform/Aether](https://github.com/GoogleCloudPlatform/Aether) |
| [zym.md](zym.md) | Zym — embeddable bytecode scripting language | [zym-lang/zym](https://github.com/zym-lang/zym) |
| [flint.md](flint.md) | Flint — Python-ish systems language, LLVM-IR codegen via `lld` (harder to port from, since Aether emits portable C) | [flint-lang/flintc](https://github.com/flint-lang/flintc) |
| [fir.md](fir.md) | Fir — ML-family typed functional language, compiles to C | [fir-lang/fir](https://github.com/fir-lang/fir) |

Each survey was first drafted in the corresponding GitHub issue and lifted into
the repository so it stays next to the code and survives issue-tracker churn.

For a lineage note about Aether's own closure/runtime tradeoff rather than one
comparison target, see
[`../design/closure-lineage-and-runtime-tradeoffs.md`](../design/closure-lineage-and-runtime-tradeoffs.md).
