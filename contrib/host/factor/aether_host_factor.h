#ifndef AETHER_HOST_FACTOR_H
#define AETHER_HOST_FACTOR_H

int factor_run_sandboxed(void* perms, const char* code);
int factor_run(const char* code);
int factor_init(void);
void factor_finalize(void);

#endif
