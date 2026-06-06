# Sequences (`*StringSeq`)

`*StringSeq` is Aether's Erlang/Elixir-shaped cons-cell linked list of
strings. Empty list is the `NULL` pointer; each cell carries a
**cached length** so `string.seq_length` is O(1); cells are
**reference-counted** so `string.seq_cons(x, t)` and
`string.seq_cons(y, t)` share the same `t` without copying. Cells
are **immutable** after creation, so cycles can't form and the
iterative free walk can't loop.

## When to use this vs the alternatives

| Shape | Typical use | Length | Random access | Refcounts | Ships in |
|---|---|---|---|---|---|
| `string[]` | Compile-time literal arrays paired with a `count` field | not carried | O(1) | manual | std (always) |
| `*StringSeq` | Runtime-built sequences, message payloads, pattern-match walks | O(1) cached | O(n) walk | yes | `std.string` |
| `std.collections.string_list_*` | Dynamic-array shape with O(1) random access | tracked | O(1) | yes | `std.collections` |
| `AetherStringArray*` (raw `string.split` return) | Legacy interop with the existing split surface | tracked | O(1) | manual | `std.string` |

Reach for `*StringSeq` when:

- You're streaming or pattern-matching head/tail (`match s { [] -> …; [h|t] -> … }`),
- You want O(1) `cons`/`head`/`tail`/`length` and the prepend-y shape
  Erlang/Elixir programmers expect,
- You're sending the sequence across an actor boundary as a message
  field — no separate `count` companion required,
- You want structural sharing without an explicit copy.

Reach for `string[]` when the count is known at compile time. Reach
for `string_list_*` when you need O(1) random access by integer
index. The shapes co-exist; pick the one that fits the access
pattern.

## Operation complexity

| Op | Cost |
|---|---|
| `string.seq_empty()` | O(1) |
| `string.seq_cons(h, t)` | O(1) — malloc one cell, retain head + tail |
| `string.seq_head(s)` | O(1) |
| `string.seq_tail(s)` | O(1) |
| `string.seq_is_empty(s)` | O(1) |
| `string.seq_length(s)` | O(1) — read cached field |
| `string.seq_retain(s)` | O(1) |
| `string.seq_free(s)` | O(n) work, O(1) stack — iterative spine walk |
| `string.seq_from_array(arr, n)` | O(n) — builds back-to-front |
| `string.seq_to_array(s)` | O(n) — materialises an `AetherStringArray*` |
| `string.seq_reverse(s)` | O(n) — fresh independent spine |
| `string.seq_concat(a, b)` | O(\|a\|) — `a` copied, `b` shared via refcount bump |
| `string.seq_take(s, n)` | O(min(n, length)) — fresh independent spine |
| `string.seq_drop(s, n)` | O(min(n, length)) — pointer walk only, returns retained tail |

## Building, walking, freeing

```aether
import std.string

main() {
    s = string.seq_empty()
    s = string.seq_cons("c", s)
    s = string.seq_cons("b", s)
    s = string.seq_cons("a", s)        // s = a -> b -> c

    println("length=${string.seq_length(s)}")  // 3 (O(1))
    println("first=${string.seq_head(s)}")      // a

    walk(s)                                     // pattern-match
    string.seq_free(s)                          // optional — see below
}

walk(s: *StringSeq) {
    match s {
        []      -> { /* end of list */ }
        [h | t] -> {
            println(h)
            walk(t)
        }
    }
}
```

## Automatic ownership

A `*StringSeq` local is reclaimed **automatically** — the compiler frees
it on reassignment and at scope exit, so the explicit `string.seq_free(s)`
above is optional. This mirrors the heap-string ownership model:

```aether
deep = string.seq_empty()
i = 0
while i < 1000 {
    deep = string.seq_cons("x", deep)   // each prior spine freed on reassign
    i = i + 1
}
// `deep` freed here at scope exit — no leak, no manual free needed
```

It is safe to *also* call `string.seq_free(s)` yourself: the explicit
free clears the ownership tracker so the scope-exit free does not run
twice. Freeing is a refcount **decrement** (`string_seq_free`), so a
structurally-shared tail survives until its last owner is freed —
`string.seq_cons(x, t)` and `string.seq_cons(y, t)` share `t` safely, and
each rooted list can be freed independently.

A sequence that **escapes** the function — returned, captured by a
closure, or stored raw in a non-seq container (`list.add`, an actor
message field) — is *not* auto-freed; ownership transfers to the
recipient, which is responsible for the eventual `string.seq_free`.

## Pattern match

The compiler dispatches `[]` / `[h|t]` arms differently based on the
matched expression's type:

- **`*StringSeq` matched expression** → NULL-checking pointer walk.
  - `[]` arm tests `s == NULL`.
  - `[h | t]` arm tests `s != NULL` and binds `h: string = s->head`,
    `t: *StringSeq = s->tail`. Tail is typed as `*StringSeq` (not as
    an array), so recursive walks compose without an extra cast.
- **`int[]` matched expression** → existing slice-style lowering
  (untouched by this addition).

When the arm body doesn't reference `h` or `t`, the binding is
omitted but the dispatch test is still emitted — same
`pattern_needs_array` optimisation the int-array path uses.

## Combinators

The four built-in structural ops are all closure-free — they don't
take an Aether function as a parameter, so the FFI surface stays
simple. Closure-bearing siblings (`map` / `filter` / `foldl`) are
deferred to a follow-up change set once the Aether-callback-from-C
bridge is settled; until then, walk the spine with `match` and
build the result with `cons`.

```aether
import std.string

s = string.split_to_seq("a,b,c,d,e", ",")

// Reverse — fresh independent spine; freeing the result doesn't
// affect `s`.
r = string.seq_reverse(s)
println(string.seq_head(r))      // "e"

// Take the first n cells. Negative or zero n yields the empty seq.
// n exceeding length clamps to the full spine.
prefix = string.seq_take(s, 3)   // ["a", "b", "c"]

// Drop the first n cells. Returns the n-th tail of `s`, retained
// (the caller owns one ref).  Negative n returns `s` itself
// retained; n exceeding length returns the empty seq.
suffix = string.seq_drop(s, 3)   // ["d", "e"]

// take(n) ++ drop(n) round-trips the spine for any n.
rebuilt = string.seq_concat(prefix, suffix)

// concat copies the first argument and shares the second via a
// refcount bump — freeing `rebuilt` later drops one ref from
// `suffix` but `s` stays walkable from any other handle.

string.seq_free(r)
string.seq_free(prefix)
string.seq_free(suffix)
string.seq_free(rebuilt)
string.seq_free(s)
```

Two equational laws every test exercises:

```
reverse(reverse(s)) == s
take(s, n) ++ drop(s, n) == s          (for any 0 <= n <= length(s))
```

`reverse` walks the source spine forward, prepending each head onto
a fresh result spine — O(n) work, O(1) auxiliary stack. `concat`
reverses the first argument (one O(\|a\|) walk), then iteratively
conses each element back onto the second; the second argument is
shared, never walked. `take` collects the first n elements into a
reverse buffer then reverses to get them in order. `drop` is a pure
pointer walk with a single `seq_retain` at the end.

## Building from existing string-array shapes

`string.split` keeps its existing `AetherStringArray*` (`ptr`) return
shape — backwards-compatible with every caller. Two paths to migrate
to a `*StringSeq`:

```aether
// (a) Direct: split into a seq from the start.
sites = string.split_to_seq(csv, ",")

// (b) Bridge: split, then materialise into a seq.
arr = string.split(csv, ",")
sites = string.seq_from_array(arr, string.array_size(arr))
string.array_free(arr)   // seq cells already retained their own refs
```

The bridge form is useful when calling code that already produces an
`AetherStringArray*` (`string.split`, foreign FFI helpers).

The reverse — get a flat `AetherStringArray*` view of an existing
seq for legacy callers — is `string.seq_to_array(s)`. The returned
pointer is freed with `string.array_free`.

## Sending across actor boundaries

`*StringSeq` field on a message just works:

```aether
message AnalyzeBatch {
    sites: *StringSeq
    poller_id: int
}

actor PollWorker {
    receive {
        AnalyzeBatch(sites, poller_id) -> {
            walk(sites)
            // sites is borrowed from the message — actor framework
            // releases the wire when the handler returns. Don't
            // free here unless you've explicitly retained.
        }
    }
}

main() {
    w = spawn(PollWorker())
    w ! AnalyzeBatch {
        sites: ["alpha.example.com", "beta.example.com", "gamma.example.com"],
        poller_id: 1
    }
}
```

The literal `[a, b, c]` in the message-field initializer is
disambiguated by the field type:

- field typed `string[]` → static C array (existing behaviour).
- field typed `*StringSeq` → cons chain (this addition).

Both shapes are first-class; pick by what the receiver wants.

## Refcount + structural sharing

`cons(h, t)` retains both `h` (via the AetherString refcount path —
`string_retain` is a no-op on plain `char*` literals) and `t` (via
`string.seq_retain`). So:

```aether
shared = string.seq_cons("y", string.seq_cons("z", string.seq_empty()))

a = string.seq_cons("a", string.seq_retain(shared))   // a = a -> y -> z
b = string.seq_cons("b", string.seq_retain(shared))   // b = b -> y -> z
                                                       // a and b share the [y, z] tail

string.seq_free(shared)   // drop our local; a and b each still own one ref

string.seq_free(a)        // a's head frees; the [y, z] tail stays
                          //   alive because b still owns it.
string.seq_free(b)        // b's head frees, then [y, z] drops to 0
                          //   and the spine collapses.
```

`string.seq_free` walks the spine iteratively (no stack growth on
deep lists), decrementing each cell's refcount. The walk stops at
the first cell whose refcount stays > 0 after decrement — the other
owner finishes the walk later when its own free runs.

Because cells are immutable (no `set_head` / `set_tail` mutator),
**cycles can't form**, and the iterative free can't loop.

## Edge cases

| Situation | Behaviour |
|---|---|
| `string.seq_free(string.seq_empty())` | No-op (NULL-safe) |
| `string.seq_head(empty)` | Returns `""` |
| `string.seq_tail(empty)` | Returns `NULL` (still an empty seq) |
| `string.seq_length(empty)` | Returns `0` |
| `string.split_to_seq("", delim)` | Single-cell list with `""` (matches `string.split`) |
| `string.split_to_seq("ab", "abcdef")` | Single-cell list with `"ab"` (matches `string.split`) |
| Trailing delimiter `"a,b,"` | Three cells: `"a"`, `"b"`, `""` |
| Leading delimiter `",a,b"` | Three cells: `""`, `"a"`, `"b"` |
| Deep list (10k+ cells) | `free` runs in O(n) time, O(1) stack |
| Mixed-type literal `[1, "a"]` against `*StringSeq` | Typechecker rejects (same rule as today's array literal) |

## C runtime layout

Authoritative definition lives in
`std/collections/aether_stringseq.h`:

```c
typedef struct StringSeq {
    int   ref_count;
    int   length;            /* length of THIS list (head + length(tail)) */
    void* head;              /* AetherString* or const char* */
    struct StringSeq* tail;
} StringSeq;
```

24 bytes per cell on 64-bit. Empty list is `NULL`. The Aether
codegen emits `#include "aether_stringseq.h"` near the prologue of
every generated TU so `*StringSeq` resolves uniformly across the
language surface.
