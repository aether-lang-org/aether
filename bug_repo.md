# aetherc 0.133.0: missing string_release on reassignment when RHS is a user-defined string-returning function

## Symptom

A simple accumulator idiom — `s = string.concat(s, X)` inside a loop, but
wrapped in a tiny user helper — leaks every intermediate value. The leak
scales with the loop body's allocation pattern: an O(N²) string-builder
loop retains O(N²) bytes; a nested O(N²) over O(N) commits retains
O(N³) bytes.

Concrete impact in the wild: `bench/bench_avn.py` (in the avn sibling
repo) hangs Ubuntu after ~245 commits. avnserver RSS climbs from 5 MB at
startup to ~14 GB monotonically over ~90 s, then the box thrashes. The
hot path is `repo_storage/rebuild.ae`'s reclist helpers — every iteration
of `rl = reclist_append_unique_(rl, ...)` and `out = string.concat(out,
...)` inside `reclist_sort` orphans the previous heap string. None are
released. By commit ~200 the per-commit retention is ~40 MB; by ~245
it's ~140 MB.

## Repro (minimal)

```aether
import std.string

my_concat(a: string, b: string) -> string {
    return string.concat(a, b)
}

main() {
    s = ""
    i = 0
    while i < 100000 {
        // (1) Direct stdlib call — release IS emitted.
        // s = string.concat(s, "a")

        // (2) Wrap it in a user helper — release is NOT emitted, leaks.
        s = my_concat(s, "a")
        i = i + 1
    }
    println(string.length(s))
}
```

Watch RSS while running both variants. Variant (1) stays flat. Variant
(2) climbs to ~5 GB before exit, because every `s = my_concat(s, "a")`
allocates a new buffer and never frees the previous `s`.

The avn case is the same shape, just one level deeper: `reclist_*`
helpers each call `string.concat` internally and return the new string;
callers reassign over their `rl` / `out` accumulator → leak.

## Generated C (avn — repo_storage/rebuild_generated.c)

```c
// reclist_sort, lines ~636-708:
const char* sorted = "";
int _heap_sorted = 0; (void)_heap_sorted;   // <-- declared, never used
while (dir_iter_next(it) == 1) {
    const char* new_sorted = "";
    while (dir_iter_next(sit) == 1) {
        new_sorted = string_concat(new_sorted, dir_entry_line(...));
        // ^ direct string_concat — wrapper IS emitted further up. OK.
    }
    sorted = new_sorted;        // <-- BUG: orphans old `sorted`,
                                //     no `if (_heap_sorted) free(...)`
}

// rebuild_dir, lines ~715-794:
const char* rl = "";
int _heap_rl = 0; (void)_heap_rl;           // <-- declared, never used
while (dir_iter_next(it) == 1) {
    rl = reclist_append_unique_(rl, name, kind, sha);
    // ^ user-defined fn returning const char* — NO release wrapper,
    //   old `rl` orphaned.
}
```

The `_heap_<var>` tracker is declared (lazily, in
`codegen_stmt.c:1110-1114`) but the only paths that *set* it to 1 or
*read* it are inside the explicit release wrapper at
`codegen_stmt.c:988-994` / `1116-1121`. When the wrapper isn't emitted,
the tracker stays a dead `int x = 0; (void)x;`.

## Root cause

`compiler/codegen/codegen_stmt.c:552-571`:

```c
static int is_heap_string_expr(ASTNode* expr) {
    if (!expr) return 0;
    if (expr->type == AST_FUNCTION_CALL && expr->value) {
        const char* fn = expr->value;
        return (strcmp(fn, "string_concat") == 0 ||
                strcmp(fn, "string_substring") == 0 ||
                strcmp(fn, "string_to_upper") == 0 ||
                strcmp(fn, "string_to_lower") == 0 ||
                strcmp(fn, "string_trim") == 0);
    }
    if (expr->type == AST_STRING_INTERP) return 1;
    return 0;
}
```

This is a hardcoded allowlist of five stdlib functions. Any other
function-call RHS — including user-defined helpers that return a fresh
heap string, plus stdlib calls the list forgot (string_copy,
string_from_int, string_repeat, etc.) — flags as non-heap, and the
release wrapper at line 1105 / 988 is skipped. Plain identifier RHS
(`sorted = new_sorted`) is also treated as non-heap, so transferring
ownership across a rename leaks the old binding too.

## Suggested fix direction

Two issues, separable:

1. **User-function returns of type `string` are unrecognised.** The
   typer already knows the return type of every function. Replace the
   hardcoded fn-name list with a check on `expr->node_type` plus an
   ownership/escape annotation: any `string`-returning AST_FUNCTION_CALL
   should be treated as heap unless the callee is annotated as
   borrow-returning (`string_length`, indexing helpers, etc.).

2. **Identifier-RHS reassignments transfer ownership without release.**
   `sorted = new_sorted;` (where `new_sorted` is heap) should either
   release the old `sorted` and steal `new_sorted`'s tracker, or move
   the tracker bit. Right now it does neither — both pointers end up
   tracked as if they alias, and the original `sorted` allocation
   leaks.

Lower-impact stopgap: widen the allowlist in `is_heap_string_expr` to
also recognise *any* AST_FUNCTION_CALL whose `node_type` resolves to
`string`. That catches the avn case (every `reclist_*` returns
`string`) and is mechanical.

A green-field fix would route this through proper escape analysis or a
linear-types-style ownership pass, but that's a bigger lift than the
allowlist widen.

## Where to look

- `compiler/codegen/codegen_stmt.c:552-571` — `is_heap_string_expr`
  predicate (the allowlist).
- `compiler/codegen/codegen_stmt.c:988-994, 1105-1121` — the release
  wrapper that's skipped when the predicate returns 0.
- `compiler/codegen/codegen_stmt.c:1351-1361` — `_heap_<name>` tracker
  initialisation at variable declaration.

## How to verify a fix

Two checks — both run against a fresh aetherc build:

1. Variant (2) of the minimal repro stays flat in RSS.

2. In the avn repo (`/home/paul/scm/AetherThings/avn`), regenerate the
   `_generated.c` files for `repo_storage/rebuild.ae` and
   `repo_storage/commit_finalise.ae`, run
   `bench/bench_avn.py --no-compress`, and confirm avnserver RSS stays
   bounded (a few hundred MB, not double-digit GB) through 5 000
   commits. The bench writes a `bench-nocompress_rss.csv` 1 Hz sampler
   line for every server-alive second — easy to plot.
