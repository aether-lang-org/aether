// aether_host_lua.c — Embedded Lua Language Host Module
//
// Embeds Lua in the Aether process. When run_sandboxed is called,
// installs the Aether sandbox checker so Lua's libc calls are
// intercepted and checked against the grant list.
//
// LOADING MODEL: dlopen, not -llua. Mirrors contrib/host/python and
// contrib/host/ruby — every Lua C-API symbol resolved via dlsym at
// first call. The bridge .a has NO unresolved Lua symbols at link
// time, so end-user binaries have no DT_NEEDED liblua and are
// ABI-portable across deploy-host Lua minor versions (5.3 / 5.4).
//
// Lua's headers use a lot of macros that are just one-line wrappers
// around real exported functions:
//
//   luaL_dostring(L,s)      → luaL_loadstring(L,s) || lua_pcall(L,0,LUA_MULTRET,0)
//   lua_pcall(L,n,r,f)      → lua_pcallk(L,n,r,f,0,NULL)
//   lua_tostring(L,i)       → lua_tolstring(L,i,NULL)
//   lua_pop(L,n)            → lua_settop(L,-(n)-1)
//   lua_pushcfunction(L,f)  → lua_pushcclosure(L,f,0)
//   luaL_checkstring(L,n)   → luaL_checklstring(L,n,NULL)
//
// We dlsym the wrapped functions and re-implement the macros as
// inline static functions over our g_lua table. The wrapper-macros
// are NOT dlsym'd directly — they don't exist as real symbols.
//
// Symbol versioning: liblua uses `lua_close@@LUA_5.4`-style
// versioning. dlsym strips version tags and returns whatever match
// it finds, so `dlsym(h, "lua_close")` works against 5.3 or 5.4
// libluas indifferently.

#include "aether_host_lua.h"
#include "../../../runtime/aether_sandbox.h"
#include "../../../runtime/aether_shared_map.h"

#ifdef AETHER_HAS_LUA
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

/* Lua 5.4+ defines luaL_openlibs as a function-like macro:
 *   #define luaL_openlibs(L) luaL_openselectedlibs(L, ~0, 0)
 * Left in place, it rewrites our struct-field call
 * `g_lua.luaL_openlibs(L)` (and the RESOLVE(luaL_openlibs, ...)
 * registration) into a reference to a non-existent
 * `luaL_openselectedlibs` member — the macOS/Homebrew Lua 5.4 build
 * break. We dlsym the real exported `luaL_openlibs` symbol, which
 * every shipping liblua provides, so undo the macro and call it as a
 * plain function pointer. (Lua 5.3 has no such macro; the #undef is a
 * harmless no-op there — matching the version-indifference this bridge
 * is built for.) */
#ifdef luaL_openlibs
#undef luaL_openlibs
#endif
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- liblua dlopen table ----------------------------------------------------

static void* liblua_handle = NULL;

static struct {
    // Lifecycle.
    lua_State* (*luaL_newstate)(void);
    void       (*luaL_openlibs)(lua_State*);
    void       (*lua_close)(lua_State*);
    // Code load + execute (the underlying fns the macros wrap).
    int        (*luaL_loadstring)(lua_State*, const char*);
    int        (*lua_pcallk)(lua_State*, int nargs, int nresults, int errfunc,
                             lua_KContext ctx, lua_KFunction k);
    // Stack manipulation (function form of lua_pop's underlying call).
    void       (*lua_settop)(lua_State*, int idx);
    // Value pushers.
    void       (*lua_pushstring)(lua_State*, const char*);
    void       (*lua_pushnil)(lua_State*);
    void       (*lua_pushcclosure)(lua_State*, lua_CFunction, int n);
    // Globals / value extraction.
    void       (*lua_setglobal)(lua_State*, const char*);
    const char* (*lua_tolstring)(lua_State*, int idx, size_t* len);
    // luaL_checkstring wrappee.
    const char* (*luaL_checklstring)(lua_State*, int arg, size_t* l);
} g_lua;

static int resolve_lua_symbols(void* h) {
#define RESOLVE(field, sym) do {                                       \
        *(void**)(&g_lua.field) = dlsym(h, sym);                       \
        if (!g_lua.field) {                                            \
            fprintf(stderr,                                            \
                "aether host_lua: liblua missing symbol %s\n", sym);   \
            return -1;                                                 \
        }                                                              \
    } while (0)

    RESOLVE(luaL_newstate,     "luaL_newstate");
    RESOLVE(luaL_openlibs,     "luaL_openlibs");
    RESOLVE(lua_close,         "lua_close");
    RESOLVE(luaL_loadstring,   "luaL_loadstring");
    RESOLVE(lua_pcallk,        "lua_pcallk");
    RESOLVE(lua_settop,        "lua_settop");
    RESOLVE(lua_pushstring,    "lua_pushstring");
    RESOLVE(lua_pushnil,       "lua_pushnil");
    RESOLVE(lua_pushcclosure,  "lua_pushcclosure");
    RESOLVE(lua_setglobal,     "lua_setglobal");
    RESOLVE(lua_tolstring,     "lua_tolstring");
    RESOLVE(luaL_checklstring, "luaL_checklstring");
    return 0;
#undef RESOLVE
}

static void* try_dlopen(const char* name) {
    if (!name || !*name) return NULL;
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

// Load liblua on first use. Strict two-step contract:
//   1. ${AETHER_LUA_SONAME} env var (orchestrator-supplied exact,
//      e.g. "liblua5.4.so.0"). Lua doesn't have a standard runtime
//      sysconfig-style probe — the orchestrator can read
//      `lua -e 'print(_VERSION)'` to learn the major.minor, then
//      check the distro's actual sonames (or just rely on the
//      unversioned symlink below).
//   2. liblua.so (Debian/Fedora unversioned symlink, when present).
// No hardcoded version-list fallback. The orchestrator owns the
// probe; the bridge stays distro-agnostic.
static int load_liblua(void) {
    if (liblua_handle) return 0;

    const char* env_soname = getenv("AETHER_LUA_SONAME");
    void* h = try_dlopen(env_soname);
    if (!h) h = try_dlopen("liblua.so");
    if (!h) {
        fprintf(stderr,
            "aether host_lua: cannot dlopen liblua "
            "(tried $AETHER_LUA_SONAME=%s, liblua.so).\n"
            "  Install a lua runtime on the host, or set "
            "AETHER_LUA_SONAME to the exact soname "
            "(e.g. liblua5.4.so.0).\n  dlerror: %s\n",
            env_soname ? env_soname : "(unset)",
            dlerror() ? dlerror() : "(none)");
        return -1;
    }

    if (resolve_lua_symbols(h) != 0) {
        dlclose(h);
        return -1;
    }
    liblua_handle = h;
    return 0;
}

// --- macro re-expressions (these are #defines in lua.h, not symbols) -------

#define LH_LUA_MULTRET (-1)  /* = LUA_MULTRET */

// luaL_dostring(L,s) → loadstring + pcall.
static int lh_dostring(lua_State* L, const char* s) {
    if (g_lua.luaL_loadstring(L, s) != 0) return 1;
    return g_lua.lua_pcallk(L, 0, LH_LUA_MULTRET, 0, 0, NULL);
}

// lua_pop(L,n) → lua_settop(L, -(n)-1).
static void lh_pop(lua_State* L, int n) {
    g_lua.lua_settop(L, -(n) - 1);
}

// lua_tostring(L,i) → lua_tolstring(L,i,NULL).
static const char* lh_tostring(lua_State* L, int idx) {
    return g_lua.lua_tolstring(L, idx, NULL);
}

// luaL_checkstring(L,n) → luaL_checklstring(L,n,NULL).
static const char* lh_checkstring(lua_State* L, int arg) {
    return g_lua.luaL_checklstring(L, arg, NULL);
}

// lua_pushcfunction(L,f) → lua_pushcclosure(L,f,0).
static void lh_pushcfunction(lua_State* L, lua_CFunction fn) {
    g_lua.lua_pushcclosure(L, fn, 0);
}

// --- bridge state (unchanged) ----------------------------------------------

static lua_State* L = NULL;

static void* lua_perms_stack[64];
static int   lua_perms_depth = 0;

extern int list_size(void*);
extern void* list_get_raw(void*, int);

static int pattern_match(const char* pat, const char* resource) {
    if (pat && strncmp(pat, "::ffff:", 7) == 0) pat += 7;
    if (resource && strncmp(resource, "::ffff:", 7) == 0) resource += 7;
    int plen = strlen(pat);
    int rlen = strlen(resource);
    if (plen == 1 && pat[0] == '*') return 1;
    if (plen > 1 && pat[plen-1] == '*') {
        if (strncmp(pat, resource, plen-1) == 0) return 1;
    }
    if (plen > 1 && pat[0] == '*') {
        int slen = plen - 1;
        if (rlen >= slen && strcmp(resource + rlen - slen, pat + 1) == 0) return 1;
    }
    return strcmp(pat, resource) == 0;
}

static int perms_allow(void* ctx, const char* category, const char* resource) {
    if (!ctx) return 1;
    int n = list_size(ctx);
    if (n == 0) return 0;
    for (int i = 0; i < n; i += 2) {
        const char* cat = (const char*)list_get_raw(ctx, i);
        const char* pat = (const char*)list_get_raw(ctx, i + 1);
        if (!cat || !pat) continue;
        if (cat[0] == '*' && pat[0] == '*') return 1;
        if (strcmp(cat, category) == 0 && pattern_match(pat, resource)) return 1;
    }
    return 0;
}

static int host_lua_checker(const char* category, const char* resource) {
    if (lua_perms_depth <= 0) return 1;
    for (int level = 0; level < lua_perms_depth; level++) {
        if (!perms_allow(lua_perms_stack[level], category, resource)) return 0;
    }
    return 1;
}

int lua_init(void) {
    if (L) return 0;
    if (load_liblua() != 0) return -1;
    L = g_lua.luaL_newstate();
    if (!L) return -1;
    g_lua.luaL_openlibs(L);
    return 0;
}

void lua_finalize(void) {
    if (L) {
        g_lua.lua_close(L);
        L = NULL;
    }
}

int lua_run(const char* code) {
    if (!code) return -1;
    if (lua_init() != 0) return -1;
    if (lh_dostring(L, code) != 0) {
        fprintf(stderr, "[lua] %s\n", lh_tostring(L, -1));
        lh_pop(L, 1);
        return -1;
    }
    return 0;
}

int lua_run_sandboxed(void* perms, const char* code) {
    if (!code) return -1;
    if (lua_init() != 0) return -1;
    if (lua_perms_depth >= 64) return -1;

    lua_perms_stack[lua_perms_depth++] = perms;
    aether_sandbox_check_fn prev = _aether_sandbox_checker;
    _aether_sandbox_checker = host_lua_checker;

    int result = 0;
    if (lh_dostring(L, code) != 0) {
        fprintf(stderr, "[lua] %s\n", lh_tostring(L, -1));
        lh_pop(L, 1);
        result = -1;
    }

    _aether_sandbox_checker = prev;
    lua_perms_depth--;

    return result;
}

// --- Shared map native bindings for Lua ------------------------------------

static uint64_t current_map_token = 0;

static int lua_aether_map_get(lua_State* state) {
    const char* key = lh_checkstring(state, 1);
    const char* val = aether_shared_map_get_by_token(current_map_token, key);
    if (val) {
        g_lua.lua_pushstring(state, val);
    } else {
        g_lua.lua_pushnil(state);
    }
    return 1;
}

static int lua_aether_map_put(lua_State* state) {
    const char* key = lh_checkstring(state, 1);
    const char* val = lh_checkstring(state, 2);
    aether_shared_map_put_by_token(current_map_token, key, val);
    return 0;
}

static void register_map_bindings(lua_State* state) {
    lh_pushcfunction(state, lua_aether_map_get);
    g_lua.lua_setglobal(state, "aether_map_get");
    lh_pushcfunction(state, lua_aether_map_put);
    g_lua.lua_setglobal(state, "aether_map_put");
}

int lua_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) {
    if (!code) return -1;
    if (lua_init() != 0) return -1;
    register_map_bindings(L);

    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(map_token);

    current_map_token = map_token;

    if (lua_perms_depth >= 64) return -1;

    lua_perms_stack[lua_perms_depth++] = perms;
    aether_sandbox_check_fn prev = _aether_sandbox_checker;
    _aether_sandbox_checker = host_lua_checker;

    int result = 0;
    if (lh_dostring(L, code) != 0) {
        fprintf(stderr, "[lua] %s\n", lh_tostring(L, -1));
        lh_pop(L, 1);
        result = -1;
    }

    _aether_sandbox_checker = prev;
    lua_perms_depth--;
    current_map_token = 0;

    return result;
}

#else
#include <stdio.h>
int lua_init(void) {
    fprintf(stderr, "error: contrib.host.lua not available (compile with AETHER_HAS_LUA)\n");
    return -1;
}
void lua_finalize(void) {}
int lua_run(const char* code) { (void)code; return lua_init(); }
int lua_run_sandboxed(void* perms, const char* code) { (void)perms; (void)code; return lua_init(); }
int lua_run_sandboxed_with_map(void* perms, const char* code,
    uint64_t token) {
  (void)perms; (void)code; (void)token;
  return lua_init();
}
#endif
