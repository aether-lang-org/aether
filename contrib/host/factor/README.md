# contrib.host.factor — Embedded Factor

`import contrib.host.factor` lets an Aether program embed
[Factor](https://factorcode.org) (a concatenative, stack-based language)
in-process, with a live, persistent VM:

```aether
import contrib.host.factor

r = factor.eval("USING: math ; 2 3 +")   // r = "5" — result back INTO Aether
factor.set("x", "10")                      // hand a k-v pair to Factor
_ = factor.eval("USING: namespaces math math.parser ; \
                 \"x\" get string>number 5 + number>string \"x\" set-global")
v = factor.get("x")                        // v = "15" — read it back
```

The VM is created once and reused across calls, so `set` / `eval` / `get`
share state — a k-v map handed in and read back, same process.

> **Status: experimental, needs a forked Factor.** This host is unlike the
> others — read the two caveats below before reaching for it.

## API

| Call | Effect |
|------|--------|
| `factor.eval(code) -> string` | evaluate `code` in the persistent VM; returns its captured output (`""` on failure) |
| `factor.run(code) -> int` | fire-and-forget; 0 on success |
| `factor.set(key, value)` | store a string value under `key` in the VM's global namespace |
| `factor.get(key) -> string` | read `key` back as raw text (`10`, not `"10"`); `""` if absent |
| `factor.run_sandboxed(perms, code)` | see Caveat 2 |

Like every other host, the k-v map is **string-only** — there is no int or
float channel. Numbers round-trip as their decimal text; `factor.get`
renders a stored string as its own content and a number as its digits
(Factor's `present`), matching lua/tcl/etc.

Evaluated snippets run in a **bare vocabulary** — there are no default
imports — so each snippet must name what it uses (`USING: math ; …`).

## Caveat 1 — needs the embed-api fork of Factor

The modern upstream VM has no re-entrant C embedding entry point: it once
shipped `start_embedded_factor`, since deleted. The surviving public path
(`start_standalone_factor`) takes the process over and `exit()`s — useless
for a host that needs the result back. So this bridge cannot bind a stock
distro `libfactor`.

It depends on a small **fork** that restores embedded eval —
[`aether-lang-org/factor-language`](https://github.com/aether-lang-org/factor-language),
`vm/embed_api.cpp` (one commit on top of upstream `master`):

```c
/* generic, NOT Aether-specific */
char* factor_embed_eval(const char* image_path, const char* src);
void  factor_embed_eval_free(char* result);
```

It reuses Factor's own `OBJ_EVAL_CALLBACK` / `eval>string` machinery and a
**stock `factor.image`** baked by the normal bootstrap (no custom save-image
— so the image is reproducible). The map-in/map-out interop is layered in
*this bridge* on top of the generic eval, so the fork's API stays
Aether-free and upstream-proposable.

## Caveat 2 — the in-process sandbox does not contain Factor

The other hosts run under the LD_PRELOAD libc gate, so `*_run_sandboxed`
checks each libc call against a grant list. Factor's VM does its own GC,
JIT (writable+executable code heap), signal handling and threads, which
that gate does not cleanly contain. `factor.run_sandboxed` accepts `perms`
only for signature parity and relies on the **process-level** sandbox
(`spawn_sandboxed`) for isolation, not a per-call checker. Treat embedded
Factor as trusted-ish code behind the process boundary, not as libc-gated
guest code.

## Performance

Measured on linux-x86-64 (your numbers will vary):

| | cost |
|--|--|
| **first call** (VM init + boot + callback install + 1 eval) | **~175 ms, once** |
| **warm eval** (`2 3 +`) | **~0.03 ms** (~33k/sec) |
| **k-v set+get pair** | **~0.06 ms** (~17k/sec) |

The shape that matters: a heavy **one-time** init (image boot + JIT warm-up;
~a few × a CPython cold start, far heavier than Lua), then effectively-free
steady state because the VM is persistent and JIT-native. Init is lazy — it
happens on the first `factor.*` call, not at process start.

- **Long-lived host** (server, an Aether process that evals repeatedly):
  great — amortize the ~175 ms once.
- **Fire-once-and-exit:** the init dominates; embedding is the wrong tool
  there.
- **Memory:** the image is ~140 MB on disk with a comparable resident
  footprint — far heavier than the lightweight hosts. Embedding Factor
  commits real RAM.
- **GC jitter:** Factor's generational GC can add occasional pause-time
  jitter under allocation-heavy loops (irrelevant for k-v/config use).

Each `set`/`get` parses+evals a small snippet (~0.03 ms), not a bare hash
op; fine for hundreds/thousands of keys. Dedicated C `set`/`get` that poke
the namespace without re-parsing would be where a further 10× lives, if
ever needed.

## How it loads libfactor

The bridge `dlopen`s libfactor at the deploy host's runtime — no `-lfactor`
on the link line. Discovery at first `factor.*` call (orchestrator-owned,
no hardcoded version list):

1. `${AETHER_FACTOR_SONAME}` — exact path/soname of the forked libfactor
   (e.g. `/opt/factor/libfactor.so`).
2. `libfactor.so` (unversioned, if installed).

Factor also needs its bootstrapped image; set `${AETHER_FACTOR_IMAGE}` to
the `factor.image` path. If the lib can't be loaded, `factor.*` returns the
error shape (`-1` / `""`) with an actionable message.

## Building

Not in the default `CONTRIB_HOST_LANGS` set (it needs the fork). To
experiment:

1. Build the fork's `libfactor` and bootstrap its `factor.image`:
   ```sh
   git clone https://github.com/aether-lang-org/factor-language
   cd factor-language && make linux-x86-64 factor-lib
   curl -fsSLO https://downloads.factorcode.org/images/master/boot.unix-x86.64.image
   ./factor -i=boot.unix-x86.64.image           # bootstraps factor.image
   ```
2. Build the bridge `.a` and point the env vars at the fork:
   ```sh
   gcc -c -O2 -DAETHER_HAS_FACTOR -DAETHER_HAS_SANDBOX -I<aether-root> \
       contrib/host/factor/aether_host_factor.c -o build/contrib/host_factor.o
   ar rcs build/contrib/libaether_host_factor.a build/contrib/host_factor.o
   export AETHER_FACTOR_SONAME=/path/to/factor-language/libfactor.so
   export AETHER_FACTOR_IMAGE=/path/to/factor-language/factor.image
   ```
3. `ae build` an Aether program with `import contrib.host.factor` — the
   bridge `.a` is auto-linked by the generic host-bridge scanner.
