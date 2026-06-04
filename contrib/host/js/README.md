# contrib.host.js — Embedded JavaScript (Duktape)

`import contrib.host.js` lets an Aether program embed the Duktape
ES5.1 engine in-process: `js.run_sandboxed(perms, "<source>")`
evaluates JavaScript with permission-checked access controls.

## How it loads libduktape

The bridge `dlopen`s libduktape at the **deploy host's** runtime —
there is no `-lduktape` on the link line. The produced binary works
against whatever Duktape minor version the deploy host has.

Discovery order at first call to `js.run` (strict two-step):

1. `${AETHER_JS_SONAME}` env var (orchestrator-supplied exact,
   e.g. `libduktape.so.207`).
2. `libduktape.so` (Debian-style unversioned symlink).

If both fail, `js.run*` returns -1 with a clear error naming the
env var. **There is no hardcoded version fallback list** — the
bridge stays distro-agnostic; the orchestrator owns the probe.

Duktape is embedded-only (no `duktape` CLI to probe with). The
orchestrator can find the soname via `ldconfig` or distro
package-manager queries:

```sh
AETHER_JS_SONAME=$(ldconfig -p | awk '/libduktape\.so/{print $1; exit}')
```

## What `ae build` does for you automatically

When your program has `import contrib.host.js`, `ae build`:
1. Links `libaether_host_js.a` (the in-tree bridge) automatically.
2. Does NOT add any libduktape link flags — the bridge dlopens
   libduktape at runtime.

`ae build` errors with a clear actionable message if the bridge .a
hasn't been built: build the image with
`aether-build --with=js --rebuild-image` or
`make contrib MODULES=js && make install-contrib`.

## `aether.toml` — usually empty for js

```toml
# nothing required for contrib.host.js
[[bin]]
name = "myapp"
path = "myapp.ae"
```

If the deploy host's duktape isn't discoverable via the default
order, set `AETHER_JS_SONAME`:

```sh
AETHER_JS_SONAME=libduktape.so.207 ae build myapp.ae
```

## Usage

```aether
import contrib.host.js

main() {
    js.run("print('hello from js: duktape ' + Duktape.version);")
}
```

## Containment model

Duktape implements ES5.1. No ambient capabilities — all functions
(`env`, `readFile`, `fileExists`, `writeFile`, `exec`) are native
bindings with sandbox checks. No LD_PRELOAD needed. Purest
containment model of the host bridges.

## Implementation notes

- All Duktape C-API access goes through a `dlsym` function-pointer
  table — see `aether_host_js.c`. The bridge .a has NO unresolved
  Duktape symbols (`nm -u` confirms).
- Three convenience macros are re-expressed as inline helpers
  over the dlsym'd underlying functions:
  - `duk_create_heap_default()` → `duk_create_heap(NULL, NULL, NULL, NULL, NULL)`
  - `duk_peval_string(c, s)` → `duk_eval_raw(c, s, 0, FLAGS)`
  - `duk_safe_to_string(c, i)` → `duk_safe_to_lstring(c, i, NULL)`
- The `DUK_COMPILE_*` flag constants used by `duk_peval_string` are
  baked directly (public ABI of Duktape's compile-flags interface;
  same shape as Ruby's `Qnil`-as-literal in that bridge).
