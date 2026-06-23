# contrib.host.racket — Embedded Racket (and the shared VM for Rhombus)

`import contrib.host.racket` lets an Aether program embed
[Racket](https://racket-lang.org) — a dynamically-typed, garbage-collected
member of the **Lisp/Scheme family** (it began life as PLT Scheme, a descendant
of Scheme, itself a descendant of Lisp) — in-process, with a live, persistent
VM. Racket's distinguishing trait is *language-oriented programming*: hygienic
macros and a `#lang`-based "tower of languages" let it host whole new surface
languages on one runtime (Rhombus is one — see
[contrib.host.rhombus](../rhombus/)). It runs on **Racket CS**, whose engine is
Chez Scheme (a high-performance, natively-JIT-compiling Scheme), which is why
embedding it boots a multi-megabyte image and commits real RAM (see
[Performance](#performance)):

```aether
import contrib.host.racket

// evaluate-with-result: fib(10) computed in the VM, value back INTO Aether.
// Use a <<HEREDOC — no interpolation, no escaping — so the snippet reads
// like real Racket. (The call is `evaluate`, not `eval` — see the API note.)
r = racket.evaluate(<<RACKET
(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
(display (fib 10))
RACKET
)                                              // r = "55"

racket.set("answer", "42")                     // hand a k-v pair to Racket
v = racket.get("answer")                       // v = "42" (read it back)
```

The VM is created once and reused across calls, so `set` / `run` / `evaluate` /
`get` share state — a scalar k-v map handed in and read back, same process.
**Rhombus runs on this same VM** — see [contrib.host.rhombus](../rhombus/) —
so a key set here is visible to `rhombus.get` and vice versa.

For map-shaped interop there is also a **first-class shared map**
(`run_sandboxed_with_map`), the same mechanism the other hosts use — see
[Shared map](#shared-map-first-class-map-interop) below.

> **Status: experimental, needs a built Racket CS.** This host is unlike the
> lightweight ones (lua/tcl/…) — read the two caveats before reaching for it.

## API

| Call | Effect |
|------|--------|
| `racket.evaluate(code) -> string` | evaluate `code` in the persistent VM; returns its **captured stdout** (`""` if the VM can't load; on a snippet error, the error message prefixed `error:` — see below) |
| `racket.run(code) -> int` | fire-and-forget; 0 on success |
| `racket.set(key, value)` | store a string value under `key` in the VM's k-v hash |
| `racket.get(key) -> string` | read `key` back as raw text; `""` if absent |
| `racket.run_sandboxed(perms, code)` | see Caveat 2 |
| `racket.run_sandboxed_with_map(perms, code, map_token)` | run `code` with live access to an Aether-owned shared map — see below |
| `racket.init() -> int` | force VM boot now (otherwise lazy on first call); 0 on success |
| `racket.finalize()` | no-op teardown (VM lives to process exit) |

The result-returning call is `evaluate`, **not `eval`** — the statically-linked
`libracketcs.a` exports its own C symbol `racket_eval`, so the bridge can't
also define one.

Like every other host, the k-v map is **string-only** — there is no int or
float channel. Numbers round-trip as their decimal text.

`racket.evaluate` captures **stdout**, not the value of the last expression: the
snippet must *print* whatever it wants back (`(display …)`, `(displayln …)`,
`(write …)`). A bare `(+ 2 3)` evaluates but prints nothing, so `eval` returns
`""` — render it: `(display (+ 2 3))`. On a snippet error (unbound identifier,
contract violation, read error, …) the bridge catches it and returns the
**error message** (prefixed `error: `) as the output, so you get a diagnostic
string rather than a crash — handy for debugging, but check for the prefix if
you treat a non-empty result as success.

Each `eval` reads **all** s-expressions from the source and evaluates them in
one shared namespace that persists across calls, so a `(define …)` in one call
is visible in the next.

## Shared map (first-class map interop)

`racket.run_sandboxed_with_map(perms, code, map_token)` gives the Racket script
a **live view of an Aether-owned shared map** — the same `aether_shared_map`
mechanism the other hosts (lua/python/…) use. Aether builds the map, the Racket
script reads and writes it *as it runs* through two procedures the bridge binds
into the namespace, and Aether reads the whole map back afterwards — including
keys the script **discovers at runtime**:

```racket
(aether-map-get "key")          ; -> value string, or "" if absent (inputs are
                                ;    frozen; writes to them are rejected)
(aether-map-put "key" "value")  ; write/overwrite an output key
```

Because Aether owns the map, key discovery is free: after the run, enumerate
with `map.keys(m)` (`std.collections`). This is the right tool when the **guest
computes the keyset** — e.g. a word-frequency map-reduce over text whose keys
aren't known upfront. The two procedures are bound automatically; you do not
add a `require`.

Choose by shape: scalar `set`/`get` for a few **known** keys; the shared map
when the guest **produces** the keyset.

## Caveat 1 — needs a built Racket CS, and STATIC-links it

Racket CS has **no shared `libracketcs.so`** to dlopen — upstream's
`cs/c/configure` explicitly refuses `--enable-shared`. Embedding is by
**static-linking** the `libracketcs.a` from a built Racket CS tree, together
with its boot images (`petite.boot` / `scheme.boot` / `racket.boot`). None of
this ships in a stock apt/brew package. And because the Chez value model is
macro-based (`Snil`, `Scons`, `Smake_bytevector` are tagged-pointer macros, not
linkable functions), the bridge's real path also needs the embedding **headers**
at compile time.

So this bridge is unlike the lightweight dlopen hosts on two axes: its archive
is **only built where the Racket CS headers are present** (probe:
`$AETHER_RACKET_INCLUDE` holding `chezscheme.h` + `racketcs.h`), and any program
that imports it **links `libracketcs.a` statically** (`ae` adds it to the link
line from `$AETHER_RACKET_LIB`, with `-rdynamic` and the runtime's system deps).
Where the headers are absent `make contrib` SKIPs the archive and `import
contrib.host.racket` fails at link with a clear "bridge .a missing" message.

Build Racket CS, then point the four env vars at it:

```sh
# Build Racket CS (produces lib/libracketcs.a + the three boot images):
git clone https://github.com/racket/racket
cd racket && make cs

# Compile-time (headers) + link-time (static archive) + runtime (boot images):
export AETHER_RACKET_INCLUDE=$PWD/racket/include          # chezscheme.h, racketcs.h, racketcsboot.h
export AETHER_RACKET_LIB=$PWD/racket/lib/libracketcs.a    # static-linked into the importer
export AETHER_RACKET_BOOT_DIR=$PWD/racket/lib             # petite.boot / scheme.boot / racket.boot
# collections dir — needed for most `racket` programs and REQUIRED for Rhombus:
export AETHER_RACKET_COLLECTS=$PWD/racket/collects
```

(Exact subpaths vary by Racket version/platform; `racket -e '(display
(path->string (find-system-path (quote collects-dir))))'` and a `find` for
`*.boot` / `libracketcs.a` pin them. A from-source `make cs` puts them under
`racket/{include,lib,collects}` as above.)

## Caveat 2 — the in-process sandbox does not contain Racket

The lightweight hosts run under the LD_PRELOAD libc gate, so `*_run_sandboxed`
checks each libc call against a grant list. Racket's VM does its own GC, JIT
(writable+executable code), signal handling and threads, which that gate does
not cleanly contain. `racket.run_sandboxed` accepts `perms` only for signature
parity and relies on the **process-level** sandbox (`spawn_sandboxed`) for
isolation, not a per-call checker. Treat embedded Racket as trusted-ish code
behind the process boundary, not as libc-gated guest code.

## Performance

The shape that matters: a heavy **one-time** init (boot the three images + JIT
warm-up — comparable to a CPython cold start, heavier than Lua/Tcl), then
effectively-free steady state because the VM is persistent and JIT-native. Init
is lazy — it happens on the first `racket.*` call (or an explicit
`racket.init()`), not at process start.

- **Long-lived host** (server, repeated evals): great — amortize boot once.
- **Fire-once-and-exit:** init dominates; embedding is the wrong tool there.
- **Memory:** the boot images + collections commit real RAM (tens of MB), far
  heavier than the lightweight hosts.
- **Rhombus:** its `#lang` package graph is large and loaded **lazily** on the
  first `rhombus.*` call — a second, bigger one-time cost on top of the Racket
  boot. Pay it once in a long-lived host; avoid in fire-once tools.

## How it links and boots

`libracketcs.a` is **static-linked** into the importing program — there is no
shared lib and no dlopen. When a program `import`s `contrib.host.racket`, `ae`
adds the archive from `${AETHER_RACKET_LIB}` to the link line (with `-rdynamic`
and the Racket runtime's system deps: `-lm -ldl -lpthread -lz -lncurses`). If
`$AETHER_RACKET_LIB` is unset the link fails with an `undefined reference to
racket_boot` error naming the missing library.

At first `racket.*` call (lazy; or an explicit `racket.init()`) the VM boots
from the three images in `${AETHER_RACKET_BOOT_DIR}`. If that dir is unset,
`racket.*` returns the error shape (`-1` / `""`) with a message naming the env
var.

## Building

Not in the default `CONTRIB_HOST_LANGS` set (it needs a built Racket CS). To
build the archive, set `$AETHER_RACKET_INCLUDE` to the built Racket's include
dir and:

```sh
AETHER_RACKET_INCLUDE=/path/to/racket/racket/include make contrib MODULES=racket
```

Then `ae build` an Aether program with `import contrib.host.racket`, with
`$AETHER_RACKET_LIB` set — the bridge `.a` is auto-linked by the host-bridge
import scanner, and `libracketcs.a` is added alongside it. `import
contrib.host.rhombus` links the **same** archive (it carries both the
`racket_*` and `rhombus_*` ABI; Rhombus is a `#lang` on this VM).

## Testing

The end-to-end test lives at
[`tests/integration/host_racket/`](../../../tests/integration/host_racket/) —
[`uses_racket.ae`](../../../tests/integration/host_racket/uses_racket.ae) is the
driver (the fib(10)=55 set-piece, the same shape as
[contrib.host.factor](../factor/)'s test: it evaluates Racket, checks the error
contract, and round-trips a scalar k-v map), and
[`test_host_racket.sh`](../../../tests/integration/host_racket/test_host_racket.sh)
is the runner.

Like the build, the test gates on the four `$AETHER_RACKET_*` env vars above: it
**SKIPs** (never fails) when they're unset — so it no-ops on CI machines without
a built Racket CS. With them set it compiles the driver with `aetherc`, links
the bridge + `libracketcs.a`, runs it, and asserts the `PASS:` line. Run it
directly once the env vars point at your Racket CS tree:

```sh
AETHER_RACKET_INCLUDE=/path/to/racket/racket/include \
AETHER_RACKET_LIB=/path/to/racket/racket/lib/libracketcs.a \
AETHER_RACKET_BOOT_DIR=/path/to/racket/racket/lib \
AETHER_RACKET_COLLECTS=/path/to/racket/racket/collects \
  tests/integration/host_racket/test_host_racket.sh
```
