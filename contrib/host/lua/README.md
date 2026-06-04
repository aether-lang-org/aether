# contrib.host.lua — Embedded Lua

`import contrib.host.lua` lets an Aether program embed Lua in-process:
`lua.run_sandboxed(perms, "<source>")` evaluates Lua with
permission-checked access controls.

## How it loads liblua

The bridge `dlopen`s liblua at the **deploy host's** runtime — there
is no `-llua` on the link line. The produced binary works against
whatever Lua minor version the deploy host has (5.3 or 5.4 supported).

Discovery order at first call to `lua.run`:
1. `${AETHER_LUA_SONAME}` env var (orchestrator-supplied exact).
2. `liblua.so` (Fedora-like unversioned).
3. Fallback: `liblua5.4.so.0` / `liblua5.3.so.0` (Debian) +
   `liblua5.4.so` / `liblua5.3.so` (Debian -dev symlinks) +
   `liblua-5.4.so` / `liblua-5.3.so` (Fedora-versioned).

If none load, `lua.run*` returns -1 with a clear stderr message
naming the env-var escape hatch.

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
