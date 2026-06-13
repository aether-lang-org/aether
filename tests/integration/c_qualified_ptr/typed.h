/* The C header that the Aether-owned / extern-imported symbols below
 * must match EXACTLY (#703). Stands in for pqsort.h / dict.h: a public
 * header every caller compiles against, with const-qualified and typed
 * pointer parameters. Force-included into the generated .gen.c by the
 * test harness; the whole point is that aetherc's emitted prototypes
 * agree with these, so gcc raises no conflicting-declaration error —
 * which restores C's cross-check that an Aether-owned definition still
 * matches the prototype every caller uses. */
#ifndef C_QUALIFIED_PTR_TYPED_H
#define C_QUALIFIED_PTR_TYPED_H

#include <stddef.h>
#include <stdint.h>

/* An Aether-OWNED public symbol: Aether emits the definition; this
 * header is what callers see. The two must agree. (Comparator is a
 * `const void *` here — the v1 gate; the fn-typed-pointer form is the
 * separate function-pointer-fields ask.) */
void aesort(void *a, size_t n, size_t es, const void *cmp,
            size_t lrange, size_t rrange);

/* Direct externs of const-taking C functions. */
uint64_t myhash(const void *key, size_t len);   /* const ptr   */
int      myf(const char *s);                     /* cstring_const */
int      myg(char *s);                           /* cstring     */

#endif
