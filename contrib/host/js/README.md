# contrib.host.js — Embedded JavaScript (alias of Duktape)

`contrib.host.js` is a **back-compat alias** for
`contrib.host.duktape` — the embedded Duktape ES5.1 JavaScript
engine. It exists only so that older code written against
`import contrib.host.js` keeps working; new code should prefer
`import contrib.host.duktape` directly, because the engine name is
the honest one and lets future runtimes (`--with=quickjs`,
`--with=v8`, …) coexist later without ambiguity.

```aether
import contrib.host.js

main() {
    js.run("print('hello from js: ' + Duktape.version);")
}
```

## How the alias works

`module.ae` is a thin re-export shim with **no C artifacts of its
own**. Each `@extern("…")` binds an Aether-side `js_*` name to the
renamed C symbol (`duktape_*`) in `libaether_host_duktape.a`. There
is no separate `libaether_host_js.a` — `ae build`'s import-driven
auto-link recognises `js` as an alias and links the duktape bridge
archive. The duktape bridge in turn `dlopen`s libduktape at the
deploy host's runtime (no `-lduktape` on the link line); see the
[duktape README](../duktape/README.md) for soname discovery,
`AETHER_DUKTAPE_SONAME`, and the containment model.

## API

All five entry points delegate to the duktape bridge's C functions:

| Aether call                          | C symbol                          | Returns |
|--------------------------------------|-----------------------------------|---------|
| `js.run(code)`                       | `duktape_run`                     | `int`   |
| `js.run_sandboxed(perms, code)`      | `duktape_run_sandboxed`           | `int`   |
| `js.run_sandboxed_with_map(perms, code, map_token)` | `duktape_run_sandboxed_with_map` | `int` |
| `js.init()`                          | `duktape_init`                    | `int`   |
| `js.finalize()`                      | `duktape_finalize`                | —       |

`perms` is a permission list pointer (e.g. `env` / `*`); the
sandboxed variants permission-check the native bindings (`env`,
`readFile`, `fileExists`, `writeFile`, `exec`).

## Shared map

`js.run_sandboxed_with_map(perms, code, map_token)` exposes a host
shared map to the script via the `aether_map_get(key)` /
`aether_map_put(key, value)` JS globals. Writes are gated by the
map token: once the host revokes the input token, further
`aether_map_put` calls are rejected, so seeded keys cannot be
tampered with.

## Building

`js` has no archive of its own — building duktape covers it:

```sh
make contrib MODULES=duktape && make install-contrib
```

or, containerised, `aether-build --with=duktape --rebuild-image`
(the alias `--with=js` is also accepted).

## Testing

There's no dedicated fib-style end-to-end test for the `js` host
(unlike factor or racket); since it is an alias, its coverage is the
duktape bridge plus the cross-host shared-map test:

- [`../../../tests/sandbox/test_shared_map_all.sh`](../../../tests/sandbox/test_shared_map_all.sh)
  — the `run_sandboxed_with_map` shared-map round-trip run across all
  host bridges. The "JS (Duktape)" case compiles
  `contrib/host/duktape/aether_host_duktape.c`, seeds a map
  (`name`=Bob, `count`=7), runs JS that reads those keys via
  `aether_map_get`, writes back `doubled` via `aether_map_put`, and
  tries to tamper with `name`. It asserts the read
  (`js:name=Bob,count=7,secret=nil`), the write (`js:result=14`), and
  that the frozen `name` stays `Bob` (`js:untampered=Bob`) after the
  token is revoked. SKIPs when Duktape's dev headers aren't installed.
- [`../../../tests/scripts/contrib_build.sh`](../../../tests/scripts/contrib_build.sh)
  — the `make contrib` smoke test. It has no `js` entry (the alias
  ships no archive); the underlying `duktape` catalogue entry verifies
  the bridge archive `libaether_host_duktape.a` compiles and archives
  cleanly. SKIPs when `pkg-config` (or the default include path) finds
  no `duktape` dev kit.
