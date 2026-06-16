/* Shim for the long-long-extern probe. These functions are spelled
 * with `long long` here — Aether's emitted C prototype for the same
 * symbol must agree, which is what the type-spelling addition tests. */

#include <stdint.h>

long long add_ll(long long a, long long b) { return a + b; }

long long make_mstime(void) { return 1718553600000LL; }

int64_t as_int64(long long x) { return (int64_t)x; }

long long from_int64(int64_t x) { return (long long)x; }
