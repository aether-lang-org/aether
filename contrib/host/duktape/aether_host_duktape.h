#ifndef AETHER_HOST_DUKTAPE_H
#define AETHER_HOST_DUKTAPE_H

#include <stdint.h>

int duktape_run_sandboxed(void* perms, const char* code);
int duktape_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token);
int duktape_run(const char* code);
int duktape_init(void);
void duktape_finalize(void);

#endif
