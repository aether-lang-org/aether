# Aether Memory Management

Aether's memory model is **deterministic scope-exit cleanup**, not garbage collection.

The guiding principle:
> **Allocations visible at call site. Cleanup explicit and composable. `defer` is your primary tool.**

No hidden allocations. No GC pauses. No magic.

### Why `defer`?

1. **Visible** -- you can see every allocation and its cleanup in the source
2. **Composable** -- works with any function, not just stdlib types
3. **Predictable** -- no special naming conventions, no hidden registry, no surprises
4. **Familiar** -- same pattern as Go's `defer` and Zig's `defer`

The one-line cost (`defer type.free(x)`) is the price for knowing exactly what your program does.

---

## The Actual Model

Aether uses two allocation mechanisms:

| Layer | What | When |
|-------|------|------|
| **Actor arena** | Actor state, message queues | Freed when actor is destroyed |
| **Stdlib heap** | `map.new()`, `list.new()`, etc. | Freed via `defer` or explicit `.free()` call |

There is **no garbage collector**.

### Arena vs defer: when to use which

They solve different problems and are used in different layers:

| | Arena | defer |
|---|--------|--------|
| **What** | A region of memory; many small allocations share one region. One "free" destroys the whole region. | A language construct: run cleanup when leaving the current scope. |
| **Who uses it** | The **runtime** uses arenas for actor state and message queues. You don't allocate from arenas directly. | **You** use it in Aether code for stdlib types (`list.new`/`list.free`, `map.new`/`map.free`) and FFI buffers. |
| **Lifetime** | Everything in the arena lives until the arena is destroyed (e.g. when the actor is destroyed). | The resource lives until scope exit; the deferred call runs then. |
| **Typical use** | Actor internals: the runtime allocates actor state and messages from an arena; when the actor goes away, the arena is freed in one shot. No per-field frees. | Any heap allocation you make: `items = list.new(); defer list.free(items);` so the list is freed when the function (or block) exits. |

---

## Stdlib Convention

All stdlib types follow one consistent naming pattern. In Aether you call them with dot syntax; the underlying C functions use underscores (`type_new`/`type_free`):

```
type.new()    -> allocates on the heap, returns a pointer (must be freed)
type.free(t)  -> frees the allocation
```

| Module | Constructor | Destructor |
|--------|-------------|------------|
| `std.map` | `map.new()` | `map.free(m)` |
| `std.list` | `list.new()` | `list.free(l)` |
| `std.string` | `string.new()` | `string.free(s)` |
| `std.dir` | `dir.list(path)` | `dir.list_free(l)` |

**Rule**: Any function whose name ends in `_new()` or `_create()` (at the C level) returns an allocated object. Its matching `_free()` is its destructor. In Aether, use `type.new()` and `type.free(x)`.

---

## The `defer` Pattern

Aether's primary memory management pattern is `defer`: allocate, immediately defer the free, then use the resource. Cleanup runs at scope exit in LIFO order.

```aether
import std.map

main() {
    m = map.new()
    defer map.free(m)

    map.put(m, "k", "v")
    print(map.get(m, "k"))
    print("\n")
    // map.free(m) runs here (scope exit)
}
```

This is explicit, visible, and composable. It works with any function -- not just stdlib types.

### Multiple allocations

```aether
import std.list
import std.map

main() {
    m = map.new()
    defer map.free(m)

    items = list.new()
    defer list.free(items)

    // Use both...
    // At scope exit: list.free(items) runs first (LIFO), then map.free(m)
}
```

### Returning allocated values

When a function allocates and returns a value, the caller receives ownership:

```aether
import std.list

build_items(n) : ptr {
    result = list.new()
    i = 0
    while i < n {
        list.add(result, i)
        i = i + 1
    }
    return result
}

main() {
    items = build_items(10)
    defer list.free(items)

    print(list.size(items))
    print("\n")
}
```

---

## Actor State

Actor `state` variables initialized with `*.new()` outlive any single message handler. Free them in the actor's `Stop` handler (or wherever the actor is shut down):

```aether
import std.map

message Store { key: string, value: string }
message Lookup { key: string }
message Stop {}

actor Cache {
    state data = map.new()

    receive {
        Store(key, value) -> {
            map.put(data, key, value)
        }
        Lookup(key) -> {
            print(map.get(data, key))
            print("\n")
        }
        Stop() -> {
            map.free(data)
        }
    }
}
```

The actor runtime frees the actor's arena (and its internal state) when the actor is shut down.

---

## Common Mistakes

**Forgetting `defer` after allocation:**

```aether
m = map.new()
map.put(m, "k", "v")
// LEAK: m is never freed
```

Fix: always pair allocation with `defer`:

```aether
m = map.new()
defer map.free(m)
```

**Deferring before the allocation succeeds:**

`defer` registers immediately. Place it right after the allocation, not before.

**Allocating inside a loop:**

`defer` fires at scope exit, not at end of each iteration. If you allocate inside a
loop, free explicitly at the end of each iteration instead:

```aether
while i < n {
    item = list.new()
    // ... use item ...
    list.free(item)
    i = i + 1
}
```

---

## Summary: When to Use What

| Situation | Approach |
|-----------|----------|
| Typical local allocation | `defer type.free(x)` right after allocation |
| Value returned from function | Caller defers the free |
| Value passed to an actor via `!` | Actor owns it; no defer in sender |
| Actor `state` initialized with `*.new()` | Free in `Stop` handler: `map.free(data)` |

---

## String memory model (heap-string-tracker)

Strings have a more granular model than other allocations: every reassignment to a string variable that has held a heap-allocated value frees the old buffer through a compiler-emitted wrapper. You don't write `defer string.free(s)` — the compiler tracks ownership transitions automatically.

This follows the principle Bjarne Stroustrup laid out in [_How do I deal with memory leaks?_](https://www.stroustrup.com/bs_faq2.html#memory-leaks):

> ... successful techniques rely on hiding allocation and deallocation inside more manageable types. Good examples are the standard containers. They manage memory for their elements better than you could without disproportionate effort.

Aether takes the same shape — strings are the "standard container" for character data, and the compiler hides allocation and deallocation transitions behind the assignment operator. The user-visible model is "assign and reassign normally"; reclamation is the compiler's job.

### What gets freed automatically

For every string variable in a function, the compiler emits a companion `int _heap_<name>` tracker that's set to `1` after every heap-string assignment and `0` after every literal assignment. On reassignment, the wrapper `if (_heap_<name>) free(<old>)` decides whether to release the previous buffer.

```aether
s = ""                          // _heap_s = 0 (literal)
s = string.concat(s, "x")       // free(""): no — _heap_s was 0; _heap_s = 1
s = string.concat(s, "y")       // free(prev concat): yes — _heap_s was 1; _heap_s = 1
s = "literal end"               // free(prev concat): yes — _heap_s was 1; _heap_s = 0
```

The wrapper handles all four transitions (heap→heap, heap→literal, literal→heap, literal→literal) uniformly, so heap memory used by string-returning expressions is reclaimed eagerly without any user-visible `defer`.

### Heap-string sources

The compiler treats these expressions as heap-allocated:

| Expression | Reason |
|---|---|
| `string.concat(a, b)` | Stdlib — always `malloc`'d |
| `string.substring(s, i, j)` | Stdlib — always `malloc`'d |
| `string.to_upper(s)` / `string.to_lower(s)` | Stdlib — always `malloc`'d |
| `string.trim(s)` | Stdlib — always `malloc`'d |
| String interpolation `"foo ${x}"` | Compiler-allocated via `_aether_interp` |
| User-defined `-> string` function whose body provably returns heap | Structural escape analysis (see below) |

### User-defined `-> string` functions (issue #405)

A user-defined function that returns `string` is treated as heap-allocated **iff every return statement in its body yields a heap-string-expression** (recursively considering other heap-returning user functions). This rules out functions that return string literals or forward borrowed parameters — those are NOT treated as heap and the wrapper won't try to free their results.

```aether
my_concat(a: string, b: string) -> string {
    return string.concat(a, b)        // RHS is heap → my_concat returns heap
}

format_msg() -> string {
    return "constant"                  // RHS is literal → format_msg does NOT return heap
}

s = my_concat("a", "b")               // _heap_s = 1
s = my_concat(s, "c")                 // free(prev); _heap_s = 1
s = format_msg()                       // free(prev); _heap_s = 0  — literal preserved
```

The recursive walk has cycle detection (mutual recursion through `-> string` user functions returns "not heap" conservatively, which is the safe answer when the structural analysis can't decide).

### Cross-block reassignment (the architectural piece of #405)

`_heap_<name>` trackers are emitted at **function-entry scope**, not at the C scope where the variable is first assigned. This means a string variable first-assigned in one if-branch and reassigned in another — or first-assigned at the top of a function and reassigned inside a deeply-nested loop — has a tracker visible at every reassignment site:

```aether
result = ""                            // _heap_result = 0 at function entry
if cond1 {
    result = my_concat("a", "b")       // _heap_result = 1; tracker is at fn scope
} else if cond2 {
    result = my_concat("c", "d")       // free(""): no; _heap_result = 1
}
// `result` is heap-allocated here regardless of which branch ran;
// scope-exit cleanup uses _heap_result to free correctly.
```

Pre-fix, the second branch couldn't see the first branch's tracker (it was C-scoped to the first `if` body) and the build failed with `'_heap_result' undeclared`. The function-entry hoist closes that scope mismatch.

### Tuple destructures (issue #420)

The same wrapper fires when a heap string is unpacked from a tuple. For user-defined tuple-returning functions the compiler runs a **per-position structural escape analysis** that mirrors the single-value case: it walks every `return e1, e2, e3` statement and AND-folds the heap-classification of each expression, per position. Position 0 is heap iff every return-site's first expression is heap; position 1 iff every return-site's second; and so on.

```aether
build_pair(prefix: string, name: string) -> (string, string) {
    return string.concat(prefix, name), string.concat(name, prefix)
    //     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^  ^^^^^^^^^^^^^^^^^^^^^^^^^^
    //     position 0 = heap            position 1 = heap
}

a = ""                              // hoisted; _heap_a = 0
b = ""                              // hoisted; _heap_b = 0
i = 0
while i < 1000 {
    a, b = build_pair("p_", "n_")   // wrapper fires per position:
    //                              // free(prev a) if heap; _heap_a = 1
    //                              // free(prev b) if heap; _heap_b = 1
    i = i + 1
}
```

Without the tuple-destructure wrapper the loop would leak 1000×2 heap allocations; with it, steady-state retention is two strings.

### `@heap` / `@borrow` annotations on tuple-returning externs

For C externs there is no body to walk, so the compiler exposes per-position annotations:

```aether
extern decode_b64(b64: string) -> (string @heap, int, string)
//                                 ^^^^^^^^^^^^       ^^^^^^
//                                 fresh malloc       borrow / static literal
//                                 (auto-free)        (no auto-free)
```

Default for unannotated string positions is `@borrow` — preserves the silent pre-#420 behaviour for existing tuple-returning externs. Adding `@heap` to a position is a behaviour change: the wrapper starts auto-freeing previous values on reassignment, so any caller currently doing manual `string.release` against the returned pointer must drop that call when adopting the annotation.

Mix-and-match is allowed; trailing positions default to borrow:

```aether
extern realpath_raw(path: string) -> (string @heap, int, string)
extern get_pair(s: string)        -> (string @heap, string @borrow)
```

### When you DO need explicit cleanup

Strings returned from a function whose ownership the compiler can't infer (e.g. an opaque C extern returning `char*` without an `@heap` annotation) need the usual `defer free(s)` pattern — same as any other heap allocation. The automatic tracker covers in-Aether assignments and annotated extern returns only.

---

## Examples

See the following runnable examples:

- [examples/basics/memory_defer.ae](../examples/basics/memory_defer.ae) -- defer pattern (recommended)
- [examples/basics/memory_manual.ae](../examples/basics/memory_manual.ae) -- manual free pattern
- [examples/basics/memory_escape.ae](../examples/basics/memory_escape.ae) -- returning allocated values
