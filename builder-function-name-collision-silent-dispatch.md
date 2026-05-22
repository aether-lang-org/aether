# `builder` and plain function sharing a name silently collide (wrong dispatch, no diagnostic)

**Reporter**: aeb session (Claude, 2026-05-22)
**Toolchain**: ae 0.177.0 (aetherc 0.177.0)
**Severity**: medium — compiles clean, runs the wrong function, no
warning. Cost ~1h of debugging in aeb's `lib/ruby`.

## What's wrong

When a module declares a plain function and a `builder` with the
**same name**, both mangle to the same C symbol (`<module>_<name>`).
aetherc does not reject the duplicate. The two declarations coexist
at the Aether level (distinct declaration kinds), but at C-emit time
one definition silently wins and **every call site dispatches to the
winner** — including call sites that meant the other one.

Across an `import` boundary there is no diagnostic at all: the
consumer compiles clean, links clean, and runs the wrong function
body with exit 0.

In aeb this bit `lib/ruby`: a `gem(_ctx, line)` setter (appends a
line to the in-memory Gemfile) and a `builder gem(ctx)` (runs
`gem build`) both mangled to `ruby_gem`. `ruby.gem(b) { ... }` in a
`.build.ae` silently invoked the *setter* and skipped the gem build
entirely — no error, the build just reported success without
producing a `.gem`. Worked around by renaming the builder to
`package` (matching `python.package`), but the compiler should have
caught it.

## Minimal reproduction

`mylib/module.ae`:

```aether
import std.string
import std.map

extern println(s: string)

// Setter: plain function named `gem`.
gem(_ctx: ptr, line: string) {
    _e = map.put(_ctx, "gems", line)
    println("SETTER ran")
}

// Builder: same name. Mangles to mylib_gem, same as the setter.
builder gem(ctx: ptr) {
    println("BUILDER ran")
}
```

`consumer.ae`:

```aether
import std.map
import mylib
import mylib (gem)

extern println(s: string)

main() {
    m = map.new()
    mylib.gem(m)   // intent: call the builder
}
```

```
$ ae build --lib . consumer.ae
Building consumer.ae...
Built: consumer
$ ./consumer
SETTER ran            # <-- builder body never ran; no warning; exit 0
```

## Same-file variant (different, slightly better)

Compiling both declarations in a single file *does* surface an
error, but a confusing one — the builder (1 arg) shadows the setter
(2 args), so a 2-arg call fails with an arity error rather than a
"duplicate definition":

```
error[E0200]: Function 'gem' expects 1 argument(s), got 2
  --> collide.ae:19:9
   |     gem(m, "rspec")
```

So the single-file path treats the two as one symbol (builder wins);
the cross-module path silently dispatches to whichever the linker
resolves first.

## Expected behavior

Reject the collision at definition time, in both the single-file and
cross-module cases:

```
error: duplicate definition of 'gem' in module 'mylib'
  a builder and a function cannot share a name (both emit C symbol 'mylib_gem')
  --> mylib/module.ae:13:9   (builder)
  note: previous definition here
  --> mylib/module.ae:7:1    (function)
```

A definition-site error is far better than either the misleading
arity error (same-file) or the silent wrong-dispatch (cross-module).

## Notes / context

- aeb already documents the *constraint* informally ("a builder must
  not share a name with a function in its module", LLM.md), precisely
  because the compiler doesn't enforce it. Enforcing it in aetherc
  would let that note be deleted.
- The general case is broader than builder-vs-function: any two
  top-level definitions that mangle to the same C symbol should error.
  The `builder` + function pair is just the case that occurs naturally
  in the SDK idiom (a setter named `x` plus a builder verb `x`).
- Related but distinct from
  `emit-lib-export-filter-drops-prefix-shadowed-symbols.md` (that one
  is about `--emit=lib` type-info dropping by name-prefix; this one is
  about exact-name C-symbol collision and runtime dispatch).
