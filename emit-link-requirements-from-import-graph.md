# Ask: emit link requirements from the import graph

**Status: request + design sketch, from the aether-ui project.**
Filed 2026-07-20 after three separate `undefined reference` hunts in one
weekend, all with the same root cause. House precedent: the
`builder-ctx-handle-void-ptr-int-conversion.md` ask → PR #725.

## The problem

`aetherc` resolves the full import graph of a program — it knows, exactly
and early, that a source uses `std.regex`, `std.http`, `contrib.sqlite`.
Then it emits C and **throws that knowledge away**. The build system that
links the C has to rediscover, by linker error, what the compiler knew:

| Import | Needs at link | How we found out |
|---|---|---|
| `std.regex` | `-lpcre2-8` | `pcre2_match_8` undefined — vg apps failed to build on MinGW until build.sh grew the flag (and 1896e5f0 added `-DPCRE2_STATIC`) |
| `std.http` | `-lssl -lcrypto` | `SSL_free` undefined — LisMusic's manual build failed mid-CI-arc |
| `contrib.sqlite` | `-laether_sqlite -lsqlite3` | known from the docs, but encoded nowhere machine-readable — LisMusic is the one aether-ui app that cannot use the generic build script, purely because of this |

Every consumer of `aetherc` output re-learns this list by hand, per
platform. aether-ui's `build.sh` now hardcodes it three different ways
(Linux and macOS lean on `ae cflags --libs`; the MinGW branch hardcodes
because that command's output isn't trusted there), and its Windows spec
matrix cannot self-build the one sqlite-using app.

## The ask (minimum viable): a `// aether-link:` header in the emitted C

When `aetherc src.ae out.c` runs, the first lines of `out.c` carry the
link requirements derived from the RESOLVED import graph:

```c
// aether-link: -laether_sqlite -lsqlite3 -lssl -lcrypto -lpcre2-8
```

- **Emit-time ground truth**: written by the same resolution that compiled
  the file, so it can never disagree with what was actually compiled.
- **Travels with the artifact**: the `.c` IS what the link consumes. (A
  sidecar file was considered and rejected: it can be deleted/left behind
  independently — aether-ui routinely `rm build/*`s and scp's generated
  `.c` between build hosts, exactly the shuffling that would orphan a
  sidecar. The comment's "cost" is a one-line grep.)
- **Self-documenting**: a human opening the generated C sees its link
  line.
- Only libraries introduced BY imports need appear; the runtime baseline
  (`-laether -pthread -lm`, winsock/bcrypt on Windows) stays where it is
  today (`ae cflags --libs`).
- Emit for the platform being compiled for; dedupe; stable order.

Consumer side becomes fully generic, e.g. in a build script:

```sh
AE_LINK="$(sed -n 's|^// aether-link:||p' "$C_FILE")"
cc ... "$C_FILE" ... $AE_LINK
```

## Second commit on the same machinery: a query form

`ae cflags --libs <src.ae>` — same resolver, answering BEFORE any
transpile. Useful for build planners (aeb's graph) and humans ("what will
this need?"). Because it shares the resolver with the emit path, the two
can never disagree. This is strictly optional sugar; the header comment
alone kills the problem.

## Where the per-module knowledge lives

Each std/contrib module that binds a native library declares its link
tokens once, next to the module (whatever form is idiomatic — a field the
resolver reads; contrib modules effectively already know this in
`contrib_build.sh`). The resolver unions the declarations over the import
closure. Modules with platform-variant deps declare per-platform tokens.

## What it fixes downstream (aether-ui evidence)

- LisMusic's "manual 2-step build" ceases to exist; the generic build
  script and the Windows spec matrix self-build it.
- The MinGW hardcoded lib list in build.sh collapses to the baseline +
  `$(grep aether-link)`.
- The entire failure class — silent, late, per-platform `undefined
  reference` archaeology for std-module native deps — dies. It cost three
  separate debugging sessions in the 2026-07-19/20 win32-parity arc alone.
