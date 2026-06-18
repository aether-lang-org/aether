#ifndef AETHER_HOST_FACTOR_H
#define AETHER_HOST_FACTOR_H

#include "../../../std/string/aether_string.h"

int factor_run_sandboxed(void* perms, const char* code);
int factor_run(const char* code);
// Evaluate `code` in the persistent VM, returning its captured output as an
// owned AetherString ("" on failure). State persists across calls.
AetherString* factor_eval(const char* code);
// Live k-v map over the persistent VM's global namespace. set returns 0 on
// success; get returns the value rendered to a string ("" if absent).
int factor_set(const char* key, const char* value);
AetherString* factor_get(const char* key);
int factor_init(void);
void factor_finalize(void);

#endif
