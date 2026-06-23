# contrib.host.lua — Embedded Lua

`import contrib.host.lua` lets an Aether program embed Lua in-process:
`lua.run_sandboxed(perms, "<source>")` evaluates Lua with
permission-checked access controls.

## How it loads liblua

The bridge `dlopen`s liblua at the **deploy host's** runtime — there
is no `-llua` on the link line. The produced binary works against
whatever Lua minor version the deploy host has (5.3 or 5.4 supported).

Discovery order at first call to `lua.run` (strict two-step):

1. `${AETHER_LUA_SONAME}` env var (orchestrator-supplied exact,
   e.g. `liblua5.4.so.0`).
2. `liblua.so` (unversioned symlink, when present).

If both fail, `lua.run*` returns -1 with a clear error naming the
env var. **There is no hardcoded version fallback list** — the
bridge stays distro-agnostic; the orchestrator owns the probe.

Lua doesn't have a standard sysconfig-style probe. The orchestrator
can read `lua -e 'print(_VERSION)'` (gives `Lua 5.4`) and derive
the soname (`liblua5.4.so.0` on Debian, `liblua-5.4.so` on Fedora).

## What `ae build` does for you automatically

When your program has `import contrib.host.lua`, `ae build`:
1. Links `libaether_host_lua.a` (the in-tree bridge) onto the
   resulting binary automatically.
2. Does NOT add any liblua link flags — the bridge dlopens liblua
   at runtime.

`ae build` errors with a clear actionable message if the bridge .a
hasn't been built: build the image with
`aether-build --with=lua --rebuild-image` (containerised) or
`make contrib MODULES=lua && make install-contrib` (installed
toolchain).

## `aether.toml` — usually empty for lua

```toml
# nothing required for contrib.host.lua
[[bin]]
name = "myapp"
path = "myapp.ae"
```

If the deploy host's Lua isn't discoverable via the default order,
set `AETHER_LUA_SONAME` in the build environment:

```sh
AETHER_LUA_SONAME=liblua5.4.so.0 ae build myapp.ae
```

## Usage

```aether
import contrib.host.lua

main() {
    lua.run("print('hello from lua')")
}
```

## Implementation notes

- All Lua C-API access goes through a `dlsym` function-pointer
  table — see `aether_host_lua.c`. The bridge .a has NO unresolved
  Lua symbols (`nm -u` confirms).
- Lua's headers `#define` many macros that wrap real functions
  (e.g. `luaL_dostring(L,s)` = `luaL_loadstring(L,s) || lua_pcall(…)`,
  `lua_pop(L,n)` = `lua_settop(L, -(n)-1)`). The bridge re-implements
  these as inline static functions over the dlopen table — only the
  underlying real functions need dlsym entries.
- Symbol versioning (`lua_close@@LUA_5.4`): dlsym strips version
  tags, so `dlsym(h, "lua_close")` works against 5.3 or 5.4
  indifferently.

## Testing

Lua has no dedicated fib-style end-to-end test like factor or racket
do. Its coverage comes from two cross-cutting tests:

- [`../../../tests/sandbox/test_shared_map_all.sh`](../../../tests/sandbox/test_shared_map_all.sh)
  — the cross-host shared-map round-trip. The Lua case seeds a shared
  map with `name=Alice` / `age=30`, runs Lua under
  `lua_run_sandboxed_with_map`, and asserts the script reads both keys
  (and gets `nil` for an absent `secret`), writes back
  `greeting=hello Alice`, and that a tamper attempt
  (`aether_map_put('name', 'TAMPERED')`) is rejected once the input
  token is revoked — `name` stays `Alice`. SKIPs when no liblua /
  `lua5.x` dev package is detected.
- [`../../../tests/scripts/contrib_build.sh`](../../../tests/scripts/contrib_build.sh)
  — the `make contrib` smoke test. Confirms the `host_lua` bridge
  archive (`build/contrib/libaether_host_lua.a`) builds from
  `aether_host_lua.c`. SKIPs the module when `pkg-config` finds no
  `lua5.4` / `lua5.3` / `lua` dev kit (hard-fails only in explicit
  `MODULES=lua` mode).
