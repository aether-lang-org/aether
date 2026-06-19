/* MIT License (https://opensource.org/licenses/MIT)
 *
 * Portions copyright (c) 2000-2026 The Legion of the Bouncy Castle Inc. (https://www.bouncycastle.org)
 *
 * Portions copyright (c) 2026 Aether Developers.
 */
/* std.bits — unsigned-bit helpers.
 *
 * Aether's `>>` on int/long is arithmetic (sign-propagating), and the
 * language has no unsigned integer types. Crypto / codec code routinely
 * needs the *unsigned* interpretation: logical right shift, rotates,
 * popcount, count-leading-zeros, and unsigned division / remainder /
 * compare. These are trivial and exact in plain C by casting to the
 * matching unsigned type, so the helpers live here and Aether calls
 * them via thin externs (std/bits/module.ae).
 *
 * Width note: Aether's `int` lowers to C `int` (32-bit on every target)
 * and `long` lowers to `long long` (64-bit on every target). All masks
 * below assume those widths. */

#include "../bits/aether_bits.h"

/* ---- Logical (unsigned) right shift ---- */

int aether_bits_lsr32(int x, int n) {
    return (int)((unsigned int)x >> (n & 31));
}

long long aether_bits_lsr64(long long x, int n) {
    return (long long)((unsigned long long)x >> (n & 63));
}

/* ---- Rotates ----
 * Mask the count by (W-1) and early-out on a zero count: shifting an
 * N-bit value by N is undefined in C, and rot-by-0 must return x. */

int aether_bits_rotr32(int x, int n) {
    unsigned int u = (unsigned int)x;
    n &= 31;
    if (n == 0) return x;
    return (int)((u >> n) | (u << (32 - n)));
}

int aether_bits_rotl32(int x, int n) {
    unsigned int u = (unsigned int)x;
    n &= 31;
    if (n == 0) return x;
    return (int)((u << n) | (u >> (32 - n)));
}

long long aether_bits_rotr64(long long x, int n) {
    unsigned long long u = (unsigned long long)x;
    n &= 63;
    if (n == 0) return x;
    return (long long)((u >> n) | (u << (64 - n)));
}

long long aether_bits_rotl64(long long x, int n) {
    unsigned long long u = (unsigned long long)x;
    n &= 63;
    if (n == 0) return x;
    return (long long)((u << n) | (u >> (64 - n)));
}

/* ---- Population count (set bits over the unsigned value) ---- */

int aether_bits_popcount32(int x) {
    unsigned int u = (unsigned int)x;
    int c = 0;
    while (u) { u &= u - 1; c++; }
    return c;
}

int aether_bits_popcount64(long long x) {
    unsigned long long u = (unsigned long long)x;
    int c = 0;
    while (u) { u &= u - 1; c++; }
    return c;
}

/* ---- Count leading zeros ----
 * Defined as W for a zero input (don't feed 0 to __builtin_clz; that's
 * undefined). Counted over the unsigned value, MSB-first. */

int aether_bits_clz32(int x) {
    unsigned int u = (unsigned int)x;
    if (u == 0) return 32;
    int c = 0;
    while ((u & 0x80000000u) == 0) { u <<= 1; c++; }
    return c;
}

int aether_bits_clz64(long long x) {
    unsigned long long u = (unsigned long long)x;
    if (u == 0) return 64;
    int c = 0;
    while ((u & 0x8000000000000000ull) == 0) { u <<= 1; c++; }
    return c;
}

/* ---- Unsigned division / remainder ----
 * Interpret both operands as unsigned. Return 0 when the divisor is 0
 * rather than trapping (callers guard their own divide-by-zero
 * semantics; a trap would be worse than a defined sentinel). */

int aether_bits_udiv32(int a, int b) {
    if (b == 0) return 0;
    return (int)((unsigned int)a / (unsigned int)b);
}

int aether_bits_urem32(int a, int b) {
    if (b == 0) return 0;
    return (int)((unsigned int)a % (unsigned int)b);
}

long long aether_bits_udiv64(long long a, long long b) {
    if (b == 0) return 0;
    return (long long)((unsigned long long)a / (unsigned long long)b);
}

long long aether_bits_urem64(long long a, long long b) {
    if (b == 0) return 0;
    return (long long)((unsigned long long)a % (unsigned long long)b);
}

/* ---- Unsigned compare ----
 * Interpret both operands as unsigned 64-bit. Returns -1 / 0 / 1. */

int aether_bits_ucmp64(long long a, long long b) {
    unsigned long long ua = (unsigned long long)a;
    unsigned long long ub = (unsigned long long)b;
    if (ua < ub) return -1;
    if (ua > ub) return 1;
    return 0;
}
