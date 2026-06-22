# contrib.host.rhombus — Embedded Rhombus

`import contrib.host.rhombus` lets an Aether program embed
[Rhombus](https://rhombus-lang.org) — a modern, general-purpose language from
the **Racket family**, designed by the Racket team (OOPSLA'23) to keep Racket's
macro power while shedding its parenthesized Lisp surface. It uses a
conventional **infix, indentation-aware syntax** (the *shrubbery* notation)
layered over Racket's hygienic-macro and module machinery, so it is dynamically
typed, garbage-collected, and fully interoperable with Racket. Concretely it is
a `#lang` on the Racket runtime — there is no separate Rhombus VM — so this host
runs on the **same embedded Racket CS (Chez Scheme) engine** as
[contrib.host.racket](../racket/), in-process, with a live, persistent VM:

```aether
import contrib.host.rhombus

// eval-with-result: the snippet runs as a #lang rhombus module body; print
// what you want back. Use a <<HEREDOC so the indentation-sensitive shrubbery
// syntax passes through verbatim.
r = rhombus.evaluate(<<RHOMBUS
fun fib(n):
  if n < 2 | n | fib(n-1) + fib(n-2)
println(fib(10))
RHOMBUS
)                                              // r = "55"

rhombus.set("answer", "42")
v = rhombus.get("answer")                      // v = "42"
```

## It is the same VM as contrib.host.racket

Rhombus is a `#lang` on the Racket runtime. This host **shares the one embedded
Racket CS VM** with [contrib.host.racket](../racket/) — same process, same
persistent state, same string-only k-v map. A key set with `rhombus.set` is
readable with `racket.get`, and vice versa. There is no second VM and no
separate bridge archive: `import contrib.host.rhombus` links the Racket bridge
(`libaether_host_racket.a`), which carries both surfaces.

Everything in the [Racket host README](../racket/README.md) applies — the
setup env vars (`AETHER_RACKET_LIB` / `AETHER_RACKET_BOOT_DIR` /
`AETHER_RACKET_INCLUDE` / `AETHER_RACKET_COLLECTS`), the **static-link** model
(`libracketcs.a`, no `.so`), the boot/eval model, the shared-map interop, and
**both caveats** (needs a built Racket CS; the in-process libc sandbox does not
contain the VM). Read it first.

## API

Identical shape to the Racket host, with `rhombus.` instead of `racket.`:

| Call | Effect |
|------|--------|
| `rhombus.evaluate(code) -> string` | evaluate `code` as a `#lang rhombus` module body in the persistent VM; returns captured **stdout** (error message prefixed `error:` on failure) |
| `rhombus.run(code) -> int` | fire-and-forget; 0 on success |
| `rhombus.set(key, value)` / `rhombus.get(key) -> string` | the shared (with Racket) string-only k-v map |
| `rhombus.run_sandboxed(perms, code)` | see Caveat 2 in the Racket README |
| `rhombus.run_sandboxed_with_map(perms, code, map_token)` | live shared-map interop (`aether-map-get` / `aether-map-put` from Racket-level glue) |
| `rhombus.init() / rhombus.finalize()` | shared VM lifecycle |

The result-returning call is `evaluate`, not `eval` (the shared bridge can't
define a C `racket_eval` — see the Racket README). The bridge wraps your source
as a `#lang rhombus` module read in memory: write top-level Rhombus forms
(definitions, `block:` bodies, `println(…)`) and `print` / `println` what you
want returned. `evaluate` captures stdout, not a value.

## Setup note specific to Rhombus

The Rhombus `#lang` and its package graph must be **installed** in the Racket
you embed:

```sh
raco pkg install rhombus      # in the same Racket CS you point the bridge at
```

and `AETHER_RACKET_COLLECTS` must include where that package landed. The
`#lang rhombus` graph is large; the bridge requires it **lazily** on the first
`rhombus.*` call, so programs that only use `racket.*` never pay for it. In a
fire-once tool the combined Racket-boot + Rhombus-require cost dominates — this
host is meant for long-lived processes that amortize it.
