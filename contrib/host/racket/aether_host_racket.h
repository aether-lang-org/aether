#ifndef AETHER_HOST_RACKET_H
#define AETHER_HOST_RACKET_H

#include <stdint.h>

#include "../../../std/string/aether_string.h"

// Shared embedded-Racket-CS bridge. Racket and Rhombus are the SAME VM —
// Rhombus is a #lang on the Racket runtime — so one persistent VM backs both
// host surfaces. The `lang` argument selects how source text is wrapped/read
// before evaluation: 0 = Racket s-expr (read + eval), 1 = Rhombus shrubbery
// (#lang rhombus module eval). State persists across calls (one VM).

// Evaluate `code` in language `lang`, returning its captured stdout as an
// owned AetherString ("" on failure / VM unavailable). Result comes back
// INTO Aether.
AetherString* racket_host_eval(int lang, const char* code);
// Fire-and-forget: evaluate for effect, discard captured output. 0 on success.
int racket_host_run(int lang, const char* code);
// Parity with the other hosts: `perms` accepted for signature symmetry; the
// Racket VM (its own GC/JIT/threads) is NOT contained by the in-process libc
// gate — rely on the process-level sandbox. See README.md.
int racket_host_run_sandboxed(int lang, void* perms, const char* code);
// First-class shared map: the guest reads/writes the Aether-owned shared map
// (by token) live via aether-map-get / aether-map-put, so runtime-discovered
// keys come back too. Enumerate afterwards with map.keys(map).
int racket_host_run_sandboxed_with_map(int lang, void* perms, const char* code,
                                       uint64_t map_token);
// Live string-only k-v map over a persistent hash in the VM. set returns 0 on
// success; get returns the value rendered to a string ("" if absent).
int racket_host_set(const char* key, const char* value);
AetherString* racket_host_get(const char* key);

int racket_host_init(void);
void racket_host_finalize(void);

// Per-language thin wrappers so the Aether module surfaces can `extern` a
// fixed-arity symbol (Aether externs can't partially-apply the `lang` arg).
// racket_* => lang 0 (Racket s-exprs); rhombus_* => lang 1 (Rhombus
// shrubbery), so the namespaced calls `racket.<verb>` / `rhombus.<verb>` map
// to these C symbols directly (factor-style bare externs — the proven contrib
// host pattern; wrapper-fn surfaces do NOT resolve under qualified calls for
// non-std modules).
//
// The result-returning call is `evaluate`, NOT `eval`: libracketcs.a (which
// the importer STATIC-links) exports its own `racket_eval`, so a C symbol
// named `racket_eval` here would be a duplicate at link. `racket_evaluate`
// sidesteps that. set/get/init/finalize are shared (one VM, one k-v hash).
AetherString* racket_evaluate(const char* code);
int           racket_run(const char* code);
int           racket_run_sandboxed(void* perms, const char* code);
int           racket_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token);

AetherString* rhombus_evaluate(const char* code);
int           rhombus_run(const char* code);
int           rhombus_run_sandboxed(void* perms, const char* code);
int           rhombus_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token);

// Shared k-v / lifecycle (each module surface externs its own names; the
// rhombus and racket variants alias the same one VM / one hash).
int           racket_set(const char* key, const char* value);
AetherString* racket_get(const char* key);
int           racket_init(void);
void          racket_finalize(void);
int           rhombus_set(const char* key, const char* value);
AetherString* rhombus_get(const char* key);
int           rhombus_init(void);
void          rhombus_finalize(void);

#endif
