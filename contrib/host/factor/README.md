# contrib.host.factor — Embedded Factor

`import contrib.host.factor` lets an Aether program embed
[Factor](https://factorcode.org) (a concatenative, stack-based language)
in-process: `factor.run("2 3 + .")` evaluates Factor source.

> **Status: experimental, needs a forked Factor.** This host is unlike
> the others — see the two caveats below before reaching for it.

## Caveat 1 — needs the embed-api fork of Factor

Upstream Factor is an image-based, GC'd, JIT VM whose only public C
embedding entry point is `start_standalone_factor` (process takeover) —
there is no "init / eval-string / result" surface the way liblua has
`luaL_dostring`. So this bridge cannot bind to a stock distro `libfactor`.

It depends on a small **fork** that adds a generic embedding API —
[`aether-lang-org/factor-language`](https://github.com/aether-lang-org/factor-language),
branch `feat/embed-api`, file `vm/embed_api.cpp`:

```c
/* generic, NOT Aether-specific */
int factor_embed_eval_oneshot(const char* image_path, const char* src);
```

which reuses Factor's existing `-e=`/`eval` word chain. The intent is to
propose this upstream (it's useful to any embedder); if accepted, this
host would bind a stock `libfactor` like the others. The map-in/map-out
interop the other hosts enjoy is deliberately **not** part of the generic
API — it can be layered on top of a later generic `factor_embed_set/get`
by any embedder, so the upstream surface stays Aether-free.

## Caveat 2 — the in-process sandbox does not contain Factor

The other hosts (lua, tcl, python, …) run under the LD_PRELOAD libc gate,
so `*_run_sandboxed` checks each libc call against a grant list. Factor's
VM does its own GC, JIT (writable+executable code heap), signal handling
and threads, which that gate does not cleanly contain. `factor_run_sandboxed`
therefore accepts `perms` only for signature parity and relies on the
**process-level** sandbox (`spawn_sandboxed`) for isolation, not a per-call
checker. Treat embedded Factor as trusted-ish code behind the process
boundary, not as libc-gated guest code.

## How it loads libfactor

The bridge `dlopen`s libfactor at the deploy host's runtime — no
`-lfactor` on the link line. Discovery at first call to `factor.run`
(orchestrator-owned, no hardcoded version list):

1. `${AETHER_FACTOR_SONAME}` — exact path/soname of the forked libfactor
   (e.g. `/opt/factor/libfactor.so`).
2. `libfactor.so` (unversioned, if installed).

Factor also needs its bootstrapped image; set `${AETHER_FACTOR_IMAGE}` to
the `factor.image` path. If the lib can't be loaded, `factor.run*` returns
-1 with an actionable error.

## Current scope

One-shot eval: each `factor.run` inits a VM, evaluates the source (Factor
prints results to stdout), and tears the VM down. A persistent VM handle
with results captured to a returned string, plus the generic key/value map
interop, are the next rungs once the Factor-side embed API grows them.

## Building

This host is not in the default `CONTRIB_HOST_LANGS` set yet (it needs the
fork). To experiment: build the `feat/embed-api` fork's `libfactor`,
bootstrap its `factor.image`, point `AETHER_FACTOR_SONAME` /
`AETHER_FACTOR_IMAGE` at them, then build the bridge `.a` and link an
Aether program with `import contrib.host.factor`.
