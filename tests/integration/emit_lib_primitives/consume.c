#include <stdio.h>
#include <stdint.h>
#include <dlfcn.h>
#include <math.h>

#define FAIL(fmt, ...) do { fprintf(stderr, "FAIL (line %d): " fmt "\n", __LINE__, ##__VA_ARGS__); return 1; } while (0)

typedef int64_t (*i64_fn)(int64_t);
typedef int32_t (*i32_fn)(int32_t);
typedef double  (*f_fn)(double, double);

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    void* h = dlopen(argv[1], RTLD_NOW);
    if (!h) FAIL("dlopen: %s", dlerror());

    i64_fn echo64 = (i64_fn)dlsym(h, "aether_echo_long");
    i32_fn negb   = (i32_fn)dlsym(h, "aether_negate_bool");
    f_fn   scale  = (f_fn)dlsym(h, "aether_scale");
    if (!echo64 || !negb || !scale) FAIL("dlsym: %s", dlerror());

    /* int64 — a value that can't fit in int32 */
    int64_t big = 1LL << 40;  /* 1 099 511 627 776 */
    int64_t got = echo64(big);
    if (got != big) FAIL("echo_long(%lld) = %lld", (long long)big, (long long)got);

    /* bool — via int */
    if (negb(0) != 1) FAIL("negate_bool(0) expected 1, got %d", negb(0));
    if (negb(1) != 0) FAIL("negate_bool(1) expected 0, got %d", negb(1));
    if (negb(42) != 0) FAIL("negate_bool(42) expected 0 (truthy), got %d", negb(42));

    /* float — Aether `float` is IEEE-754 double-precision (C double).
     * The TYPE_FLOAT codegen-consistency fix (0.[current]) made every
     * emission site lower TYPE_FLOAT to C double; the previous mix of
     * `float` and `double` spellings produced silent ABI mismatches
     * across module / FFI boundaries. */
    double s = scale(3.0, 2.5);
    if (fabs(s - 7.5) > 1e-9) FAIL("scale(3.0, 2.5) = %f, expected 7.5", s);

    dlclose(h);
    printf("OK: int64, bool, float round-trip\n");
    return 0;
}
