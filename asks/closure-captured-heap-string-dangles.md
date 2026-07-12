# Bug: a closure capturing a heap string captures a DANGLING pointer

## Summary

A closure that captures a **heap-allocated string** (anything computed —
`string.concat`, `string.from_int`, …) stores a *borrowed* `const char*` in its
env. The enclosing scope frees that string — on the next loop iteration, and
again at scope exit — while the closure is still holding it. Firing the closure
later reads freed memory and prints garbage.

No compile error. No warning. **Silent memory corruption.**

Reproduces on **v0.389.0** (current main, i.e. *after* the
closure-inside-closure capture fix `1ca4dacd`).

String **literals** are fine (they live in `.rodata`). **Ints** are fine. Only
*heap* strings are affected — which is every computed label.

## Minimal repro

```aether
import std.string
import std.list

take(cb: fn) -> ptr { return box_closure(cb) }
fire(p: ptr)        { c = unbox_closure(p); call(c) }

build_them(saved: ptr) {
    i = 0
    while i < 3 {
        nm = string.concat("item ", string.from_int(i))   // heap string
        h = take() callback { println("fired: [${nm}]") } // captures it
        list.add(saved, h)
        i = i + 1
    }
}

main() {
    saved = list.new()
    build_them(saved)
    println("--- firing AFTER build_them returned ---")
    n = list.size(saved)
    j = 0
    while j < n {
        p, _ = list.get(saved, j)
        fire(p)
        j = j + 1
    }
}
```

Expected:
```
fired: [item 0]
fired: [item 1]
fired: [item 2]
```

Actual (v0.389.0):
```
fired: [�-�c]
fired: [��zA?V]
fired: [��zA?V]
```

## Root cause (visible in the emitted C)

`nm` becomes a single **loop-carried heap slot**, freed and reassigned each
iteration; the closure env merely *borrows* the pointer:

```c
void build_them(void* saved) {
    int _heap_nm = 0;
    const char* nm = NULL;
    while (i < 3) {
        { const char* _tmp_old = nm;
          nm = string_concat("item ", ...);
          if (_heap_nm) aether_heap_str_free(_tmp_old);   // frees the string the
          _heap_nm = 1; }                                  // PREVIOUS closure holds
        h = take( ({ _closure_env_0* _e = malloc(...);
                     _e->nm = nm;                          // borrow, no copy/retain
                     ... }) );
        ...
    }
}   // scope exit frees the last one too
```

So by the time any stored closure runs, its `nm` has been freed — the earlier
ones during the loop, the last at return.

## Isolation

| # | Captured value | Fired | Result |
|---|----------------|-------|--------|
| 1 | string **literal** (`nm = "static"`) | after return | ✅ correct (`.rodata`) |
| 2 | **int** (`k = i * 100`) | after return | ✅ correct |
| 3 | **heap string** (`string.concat(...)`) | immediately, same iteration | ✅ correct (not yet freed) |
| 4 | **heap string** | after the defining function returns | ❌ **garbage** |
| 5 | **heap string**, several closures stored in a loop | after return | ❌ garbage; earlier ones clobbered mid-loop |

## Why it matters

This is the load-bearing pattern for any list/repeater UI (`ng-repeat` /
`ForEach`): build one handler per item **now**, each closing over that item's
computed label, and fire them **later** on click. It is what aether-ui's `each`
(dynamic children) needs, and it is what every app does today when it wires a
per-row callback.

It is worse than the two bugs in `nested-closure-capture-bug.md` because there
is no diagnostic at all: the program compiles clean and silently reads freed
memory. In aether-ui it showed up as a widget label rendering as
`clicked \377\024\301\242\033V`.

## Suggested fix direction

The closure env must participate in the string-ownership protocol rather than
borrowing:

- **Retain on capture:** when a captured variable is a heap string
  (the compiler already tracks this — the `_heap_<name>` flag in the emitted C),
  the env should take its own reference (`string_retain`, or copy) at
  `_aether_make_closure_N` time, and the enclosing scope's
  `aether_heap_str_free` must not free out from under it.
- Equivalently: heap-promote the captured string the way an *assigned* capture
  is already heap-promoted (per `docs/closures-and-builder-dsl.md` §Ref Cells,
  a captured local a closure assigns to is heap-promoted and shared — a
  captured heap string needs the same lifetime treatment even when only read).
- Closure envs are `malloc`'d and (per the existing `_aether_thunk_free`
  machinery) freed somewhere; the release of a retained string wants to hang
  off that.

Repro file: the snippet above (also `tests/syntax/` candidate, alongside
`test_closure_nested_capture_probe.ae` from the previous ask).
