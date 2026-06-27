# #924 / #925 / #934 — re-export, cycle diagnostics, cross-module UFCS

Three module-boundary fixes that landed together (they touch the same
import/merge/resolution machinery). Grounded against the live compiler.

## #925 — circular-import diagnostic names the actual cycle

The cycle detector (`dependency_graph_has_cycle` in `aether_module.c`) ran a
DFS from each node and, on a back edge, reported `nodes[i]->module_name` —
the DFS *root*, which is the synthetic `__main__` entry node, not a cycle
member. The location was a hardcoded `0:0`.

Fix: `dfs_find_cycle` now threads an in-stack ancestor path. On a back edge it
slices the path from the re-entered node to the current node (plus the closing
node) — the real cycle — and the caller formats `a -> b -> a`. `__main__`
never appears because nothing imports it, so it can't be re-entered. The
module names in order are the actionable signal the issue asked for; the
closing `import` file:line is a possible follow-up (would need source
locations stored on graph edges).

## #924 — first-class re-export

A consumer's `hub.X` resolves by mangling to `hub_X` and looking that symbol
up. When `X` is *re-exported* (hub lists it in `exports` but imports it from
`inner`), the defining symbol is `inner_X`, so `hub_X` doesn't exist and the
old code reported `module 'hub' has no export 'X'`.

The fix has three coordinated parts, all keyed on a single resolver
`module_resolve_reexport(module, symbol)` — "module exports symbol but doesn't
define it; find the imported module that does" (transitive, cycle-guarded, and
*precise*: a locally-defined export is never treated as a re-export, so a name
collision resolves to the local definition):

1. **Resolution** (`typechecker.c`). Both the const member-access path and the
   qualified-call path (`lookup_qualified_symbol`) fall back to the re-export
   resolver when the local `hub_X` symbol is absent, redirecting to
   `<origin>_X`. The function-call path additionally rewrites the call node's
   `value` to the origin-qualified form so codegen emits `<origin>_X`.

2. **Merge** (`module_merge_into_program`). A dedicated re-export pull-in pass,
   driven off each imported module's *export list* (re-export is opt-in via
   `exports`), clones the origin's func/const definition into the program
   under the origin namespace. Driven off exports rather than the import-graph
   BFS because a bodyless re-export hub may not have its import_count populated
   when that BFS runs.

3. **Reachability** (`module_prune_unreachable`). The dead-code prune keyed the
   reachable set on the call's spelled name (`hub.X` → `hub_X`), which no
   longer matches the cloned `<origin>_X`, so the body was pruned before
   typecheck. `prune_collect_calls` now also seeds the origin-qualified form.

Verified end-to-end including the motivating shape: a `consts` hub re-exporting
a `DERIVED` constant from a `layout_consts` leaf that imports `mqtypes` —
the `consts → mqtypes → consts` cycle never forms because the
mqtypes-importing piece is a leaf, not the hub.

## #934 — UFCS across the import boundary

#928 shipped UFCS (`x.f(args)` → `f(x, args)`) but resolved `f` only among
same-file functions (`lookup_symbol(table, method)`), so a library-provided
fluent surface (`expect(x).to_equal(5)` with the matchers in an imported
module) failed with `Undefined function`.

`try_ufcs_rewrite` now resolves the method in two tiers: (1) a same-file bare
function, then (2) any **visible imported module** that exports a `method`
whose first parameter matches `typeof(recv)` — the same visibility a normal
qualified `mod.method(recv)` call honors. On an imported match the call is
rewritten to the qualified `mod.method` form. Same-file wins over imported;
a type-mismatched receiver declines cleanly (clean `Undefined function`, no
silent coercion).

The reachability interaction matters here too: a consumer that *only* calls
`x.method()` (never a direct `mod.method(...)`) would otherwise have
`mod_method` pruned before the UFCS rewrite runs. `prune_collect_calls` seeds
every imported `mod.method` candidate for a value-receiver dot (a sound
over-approximation), so the real target survives to be resolved.

## Tests

- `tests/integration/circular_import_diag/` — cycle named `a -> b -> a`, not `__main__`.
- `tests/integration/module_reexport/` — transitive fn + const re-export through a bodyless facade.
- `tests/integration/ufcs_cross_module/` — UFCS on imported methods, stored-value and chained-call-result receivers.

(All three fixtures import sibling `lib/` modules, so they're on the Makefile
`-prune` list — they're driven by their `test_*.sh`, not collected as
standalone `.ae` compile tests.)
