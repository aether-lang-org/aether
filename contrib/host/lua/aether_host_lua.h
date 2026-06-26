#ifndef AETHER_HOST_LUA_H
#define AETHER_HOST_LUA_H

#include <stdint.h>

int lua_run_sandboxed(void* perms, const char* code);
int lua_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token);
int lua_run(const char* code);
int lua_init(void);
void lua_finalize(void);

/* #904 bidirectional embedding API. */
void* lua_vm_new(void);
void  lua_vm_free(void* vm);
int   lua_vm_register(void* vm, const char* name, void* host_fn);
void  lua_vm_set_global_str(void* vm, const char* name, const char* value);
void  lua_vm_set_global_int(void* vm, const char* name, long value);
void  lua_vm_set_global_strlist(void* vm, const char* name, void* items);
int   lua_vm_eval(void* vm, const char* code);
int   lua_vm_result_type(void* vm);
long  lua_vm_result_int(void* vm);
const char* lua_vm_result_str(void* vm);
int   lua_vm_result_bool(void* vm);
void  lua_vm_pop_result(void* vm);
int   lua_arg_count(void* vm);
int   lua_arg_type(void* vm, int i);
const char* lua_arg_str(void* vm, int i);
long  lua_arg_int(void* vm, int i);
int   lua_arg_bool(void* vm, int i);
void  lua_push_int(void* vm, long v);
void  lua_push_str(void* vm, const char* v);
void  lua_push_bool(void* vm, int v);
void  lua_push_nil(void* vm);
void  lua_push_tagged_table(void* vm, const char* field, const char* msg);
int   lua_raise_error(void* vm, const char* msg);
void  lua_vm_set_instruction_limit(void* vm, long budget, int every);

#endif
