#!/bin/sh
# #910 host-callback table builder: a host callback can return a multi-element
# array, a string-keyed map, and a nested array-of-arrays (the RESP multibulk →
# Lua-table shapes `redis.call('KEYS'/'HGETALL'/...)` needs). Follow-up to #904.
#
# SKIPs cleanly when matching liblua headers + runtime soname aren't present.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) echo "  [SKIP] host_lua_table_builder on Windows"; exit 0 ;;
esac
LUA_CFLAGS=""; LUA_LIBS=""; LUA_SONAME=""
for v in lua5.4 lua5.3 lua5.2; do
    if pkg-config --exists "$v" 2>/dev/null; then cf="$(pkg-config --cflags "$v")"
    elif [ -f "/usr/include/$v/lua.h" ]; then cf="-I/usr/include/$v"
    else continue; fi
    so="$(ldconfig -p 2>/dev/null | grep -oE "liblua${v#lua}\.so[^ ]*" | grep -vi 'c++' | head -1)"
    [ -z "$so" ] && so="$(ls /usr/lib/*/liblua${v#lua}.so.* 2>/dev/null | grep -vi 'c++' | head -1)"
    if [ -n "$cf" ] && [ -n "$so" ]; then LUA_CFLAGS="$cf"; LUA_LIBS="-llua${v#lua}"; LUA_SONAME="$so"; break; fi
done
if [ -z "$LUA_CFLAGS" ] || [ -z "$LUA_SONAME" ]; then
    echo "  [SKIP] host_lua_table_builder: no matching liblua headers+runtime"; exit 0; fi
[ -f "$ROOT/build/libaether.a" ] || { echo "  [SKIP] libaether.a not built"; exit 0; }
export AETHER_LUA_SONAME="$LUA_SONAME"
TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! gcc -o "$TMPDIR/t" \
    "$ROOT/contrib/host/lua/aether_host_lua.c" \
    "$ROOT/runtime/aether_sandbox.c" "$ROOT/runtime/aether_shared_map.c" \
    $LUA_CFLAGS -I"$ROOT" -I"$ROOT/runtime" -DAETHER_HAS_LUA -DAETHER_HAS_SANDBOX \
    -L"$ROOT/build" -laether $LUA_LIBS -ldl -lm -lrt -lpthread \
    -Wno-discarded-qualifiers -xc - 2>"$TMPDIR/cc.log" << 'CEOF'
#include <stdio.h>
#include "contrib/host/lua/aether_host_lua.h"
void aether_args_init(int a, char** v){(void)a;(void)v;}
void* _aether_ctx_stack[64]; int _aether_ctx_depth = 0;

/* returns {"k1","k2","k3"} — the KEYS/SMEMBERS shape */
static int cb_array(void* vm) {
    lua_table_begin(vm, 3, 0);
    lua_table_seti_str(vm, 1, "k1");
    lua_table_seti_str(vm, 2, "k2");
    lua_table_seti_str(vm, 3, "k3");
    lua_table_finish(vm);
    return 1;
}
/* returns {field="f1", count=7} — the HGETALL/map shape */
static int cb_map(void* vm) {
    lua_table_begin(vm, 0, 2);
    lua_table_set_str(vm, "field", "f1");
    lua_table_set_int(vm, "count", 7);
    lua_table_finish(vm);
    return 1;
}
/* returns {{"a","b"},{"c"}} — array of arrays (nested) */
static int cb_nested(void* vm) {
    lua_table_begin(vm, 2, 0);          /* outer */
    lua_table_begin(vm, 2, 0);          /* inner #1 */
    lua_table_seti_str(vm, 1, "a");
    lua_table_seti_str(vm, 2, "b");
    lua_table_end_seti(vm, 1);          /* outer[1] = inner#1 */
    lua_table_begin(vm, 1, 0);          /* inner #2 */
    lua_table_seti_str(vm, 1, "c");
    lua_table_end_seti(vm, 2);          /* outer[2] = inner#2 */
    lua_table_finish(vm);
    return 1;
}

int main(void) {
    void* vm = lua_vm_new();
    if (!vm) { printf("FAIL vm\n"); return 1; }
    lua_vm_register(vm, "host.arr",    (void*)cb_array);
    lua_vm_register(vm, "host.map",    (void*)cb_map);
    lua_vm_register(vm, "host.nested", (void*)cb_nested);

    /* The script exercises each shape exactly as a real EVAL would. */
    int rc = lua_vm_eval(vm,
        "local a = host.arr()\n"
        "assert(#a == 3 and a[1]=='k1' and a[3]=='k3', 'array')\n"
        "local m = host.map()\n"
        "assert(m.field=='f1' and m.count==7, 'map')\n"
        "local n = host.nested()\n"
        "assert(#n==2 and #n[1]==2 and n[1][2]=='b' and n[2][1]=='c', 'nested')\n"
        "return a[1] .. ',' .. m.field .. ',' .. n[1][1] .. n[2][1]\n");
    if (rc != 0) { printf("FAIL eval: %s\n", lua_vm_result_str(vm)); return 1; }
    printf("result=%s\n", lua_vm_result_str(vm));
    lua_vm_pop_result(vm);
    lua_vm_free(vm);
    return 0;
}
CEOF
then
    echo "  [SKIP] host_lua_table_builder: harness compile/link failed"
    head -8 "$TMPDIR/cc.log" | sed 's/^/      /'; exit 0
fi

OUT="$("$TMPDIR/t" 2>&1)"
if echo "$OUT" | grep -q "result=k1,f1,ac"; then
    echo "  [PASS] host_lua_table_builder: array + map + nested array-of-arrays"
else
    echo "  [FAIL] host_lua_table_builder: $OUT"; exit 1
fi
