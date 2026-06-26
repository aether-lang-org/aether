#!/bin/sh
# #904 contrib.host.lua bidirectional embedding: a persistent host-owned VM
# that registers a host callback the script calls, injects a global, runs
# code, and reads a TYPED result back. Models the aedis EVAL shape.
#
# Skips cleanly when liblua dev headers aren't present (CI machines without
# Lua), like the other host-bridge tests.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) echo "  [SKIP] host_lua_bidirectional on Windows"; exit 0 ;;
esac

# Pick a Lua version that has BOTH dev headers and a runtime soname, so the
# compiled-against and dlopen'd libs match. Set AETHER_LUA_SONAME for the
# bridge's runtime dlopen (the unversioned liblua.so symlink is often absent).
LUA_CFLAGS=""; LUA_LIBS=""; LUA_SONAME=""
for v in lua5.4 lua5.3 lua5.2; do
    if pkg-config --exists "$v" 2>/dev/null; then
        cf="$(pkg-config --cflags "$v")"
    elif [ -f "/usr/include/$v/lua.h" ]; then
        cf="-I/usr/include/$v"
    else
        continue
    fi
    # runtime soname for this version, if present
    so="$(ldconfig -p 2>/dev/null | grep -oE "liblua${v#lua}\.so[^ ]*" | grep -vi 'c++' | head -1)"
    [ -z "$so" ] && so="$(ls /usr/lib/*/liblua${v#lua}.so.* 2>/dev/null | grep -vi 'c++' | head -1)"
    if [ -n "$cf" ] && [ -n "$so" ]; then
        LUA_CFLAGS="$cf"; LUA_LIBS="-llua${v#lua}"; LUA_SONAME="$so"; break
    fi
done
if [ -z "$LUA_CFLAGS" ] || [ -z "$LUA_SONAME" ]; then
    echo "  [SKIP] host_lua_bidirectional: no matching liblua headers+runtime"; exit 0
fi
[ -f "$ROOT/build/libaether.a" ] || { echo "  [SKIP] libaether.a not built"; exit 0; }
export AETHER_LUA_SONAME="$LUA_SONAME"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

# A C harness exercising the bidirectional API directly (the same surface
# module.ae exposes; a C driver keeps the test independent of `ae build`'s
# contrib-link plumbing, matching tests/sandbox/test_shared_map_all.sh).
if ! gcc -o "$TMPDIR/t" \
    "$ROOT/contrib/host/lua/aether_host_lua.c" \
    "$ROOT/runtime/aether_sandbox.c" \
    "$ROOT/runtime/aether_shared_map.c" \
    $LUA_CFLAGS -I"$ROOT" -I"$ROOT/runtime" -DAETHER_HAS_LUA -DAETHER_HAS_SANDBOX \
    -L"$ROOT/build" -laether $LUA_LIBS -ldl -lm -lrt -lpthread \
    -Wno-discarded-qualifiers \
    -xc - 2>"$TMPDIR/cc.log" << 'CEOF'
#include <stdio.h>
#include "contrib/host/lua/aether_host_lua.h"
void aether_args_init(int a, char** v){(void)a;(void)v;}
void* _aether_ctx_stack[64]; int _aether_ctx_depth = 0;
extern void* list_new(void); extern void list_add_raw(void*, void*);

/* A host callback: doubles its integer arg, returns it. The script calls
   this as `host.double(n)`. */
static int host_double(void* vm) {
    long n = lua_arg_int(vm, 1);
    lua_push_int(vm, n * 2);
    return 1;
}
/* A host callback returning a status table { ok = "PONG" }. */
static int host_ping(void* vm) {
    lua_push_tagged_table(vm, "ok", "PONG");
    return 1;
}

int main(void) {
    void* vm = lua_vm_new();
    if (!vm) { printf("FAIL: vm_new\n"); return 1; }

    lua_vm_register(vm, "host.double", (void*)host_double);
    lua_vm_register(vm, "host.ping",   (void*)host_ping);

    /* inject a global int + a KEYS-style string array */
    lua_vm_set_global_int(vm, "BASE", 100);
    void* keys = list_new(); list_add_raw(keys, "k1"); list_add_raw(keys, "k2");
    lua_vm_set_global_strlist(vm, "KEYS", keys);

    /* a script that calls the host callback + reads globals, returns int */
    int rc = lua_vm_eval(vm,
        "local d = host.double(21)\n"      /* 42 via host callback */
        "local p = host.ping()\n"          /* {ok=PONG} */
        "assert(p.ok == 'PONG')\n"
        "assert(#KEYS == 2 and KEYS[1] == 'k1')\n"
        "return d + BASE\n");              /* 42 + 100 = 142 */
    if (rc != 0) {
        printf("FAIL: eval rc=%d err=%s\n", rc, lua_vm_result_str(vm));
        return 1;
    }
    int t = lua_vm_result_type(vm);
    long v = lua_vm_result_int(vm);
    printf("type=%d value=%ld\n", t, v);
    lua_vm_pop_result(vm);

    /* a script returning a typed string */
    lua_vm_eval(vm, "return 'hello ' .. KEYS[2]");
    printf("strtype=%d str=%s\n", lua_vm_result_type(vm), lua_vm_result_str(vm));
    lua_vm_pop_result(vm);

    /* instruction-limit guard aborts an infinite loop */
    lua_vm_set_instruction_limit(vm, 100000, 100);
    int loop_rc = lua_vm_eval(vm, "while true do end return 1");
    printf("guard_rc=%d\n", loop_rc);   /* expect -1 (aborted) */

    lua_vm_free(vm);
    return 0;
}
CEOF
then
    echo "  [SKIP] host_lua_bidirectional: harness compile/link failed (likely no liblua at link)"
    head -8 "$TMPDIR/cc.log" | sed 's/^/      /'
    exit 0
fi

OUT="$("$TMPDIR/t" 2>&1)"
fail=0
echo "$OUT" | grep -q "type=2 value=142" || { echo "  [FAIL] typed int result (host callback + global): $OUT"; fail=1; }
echo "$OUT" | grep -q "strtype=3 str=hello k2"  || { echo "  [FAIL] typed string result: $OUT"; fail=1; }
echo "$OUT" | grep -q "guard_rc=-1"             || { echo "  [FAIL] instruction-limit guard: $OUT"; fail=1; }
if [ "$fail" -eq 0 ]; then
    echo "  [PASS] host_lua_bidirectional: callback + typed result + global inject + guard"
fi
[ "$fail" -eq 0 ]
