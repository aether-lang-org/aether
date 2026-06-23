# contrib.host.tcl — Embedded Tcl

`import contrib.host.tcl` lets an Aether program embed Tcl in-process:
`tcl.run_sandboxed(perms, "<source>")` evaluates Tcl with
permission-checked access controls.

## How it loads libtcl

The bridge `dlopen`s libtcl at the **deploy host's** runtime — there
is no `-ltcl` on the link line. The produced binary works against
whatever Tcl minor version the deploy host has (8.5 / 8.6 / 9.0
supported).

Discovery order at first call to `tcl.run` (strict two-step):

1. `${AETHER_TCL_SONAME}` env var (orchestrator-supplied exact,
   e.g. `libtcl8.6.so`).
2. `libtcl.so` (Debian-style unversioned symlink, present with
   tcl-dev).

If both fail, `tcl.run*` returns -1 with a clear error naming the
env var. **There is no hardcoded version fallback list** — the
bridge stays distro-agnostic; the orchestrator owns the probe.

Orchestrator probe via tclsh:

```sh
AETHER_TCL_SONAME=$(echo 'puts libtcl$tcl_version.so' | tclsh)
```

## What `ae build` does for you automatically

When your program has `import contrib.host.tcl`, `ae build`:
1. Links `libaether_host_tcl.a` (the in-tree bridge) automatically.
2. Does NOT add any libtcl link flags — the bridge dlopens libtcl
   at runtime.

`ae build` errors with a clear actionable message if the bridge .a
hasn't been built: build the image with
`aether-build --with=tcl --rebuild-image` or
`make contrib MODULES=tcl && make install-contrib`.

## `aether.toml` — usually empty for tcl

```toml
# nothing required for contrib.host.tcl
[[bin]]
name = "myapp"
path = "myapp.ae"
```

If the deploy host's Tcl isn't discoverable via the default order,
set `AETHER_TCL_SONAME`:

```sh
AETHER_TCL_SONAME=libtcl8.6.so.0 ae build myapp.ae
```

## Usage

```aether
import contrib.host.tcl

main() {
    tcl.run("puts \"hello from tcl\"")
}
```

## Implementation notes

- All Tcl C-API access goes through a `dlsym` function-pointer
  table — see `aether_host_tcl.c`. The bridge .a has NO unresolved
  Tcl symbols (`nm -u` confirms).
- Tcl's C API is conventionally non-macro — the headers expose
  real exported `Tcl_*` functions, no `pTHX_`-style context
  passing, no struct-tag bit-twiddling. **No macro re-expressions
  needed** (unlike python's `Py_INCREF` / perl's `SvTRUE` /
  duktape's `duk_peval_string`). The cleanest of the dlopen
  rewrites.
- `TCL_OK = 0` / `TCL_ERROR = 1` constants are stable across Tcl
  8.x and 9.x.

## Testing

Automated coverage today is the contrib-build smoke test at
[`tests/scripts/contrib_build.sh`](../../../tests/scripts/contrib_build.sh) —
the `tcl|host_tcl …` catalogue entry builds `aether_host_tcl.c` (gated on its
`probe_tcl` Tcl-headers probe) so `make contrib` produces and links the
`libaether_host_tcl.a` bridge archive. It confirms the bridge compiles and has
no unresolved Tcl symbols, but exercises nothing at runtime.

There is **no dedicated end-to-end test** for tcl — no `host_tcl/` driver in
the shape of [contrib.host.racket](../racket/)'s fib(10)=55 set-piece — and tcl
is **not** in the cross-host shared-map test
([`tests/sandbox/test_shared_map_all.sh`](../../../tests/sandbox/test_shared_map_all.sh)).
Both are gaps worth filling.
