/* MIT License (https://opensource.org/licenses/MIT)
 *
 * Portions copyright (c) 2000-2026 The Legion of the Bouncy Castle Inc. (https://www.bouncycastle.org)
 *
 * Portions copyright (c) 2026 Aether Developers.
 */
#ifndef AETHER_BITS_H
#define AETHER_BITS_H

/* std.bits — unsigned-bit helpers.
 *
 * Aether's `>>` on int/long is arithmetic (sign-propagating) and the
 * language has no unsigned integer types, so the unsigned bit ops that
 * cryptography / codec code needs live here as plain-C helpers. Aether's
 * `int` is C `int` (32-bit) and `long` is C `long long` (64-bit) on
 * every target; the helpers assume those widths. */

/* Logical (unsigned) right shift. Shift count is masked by (W-1). */
int       aether_bits_lsr32(int x, int n);
long long aether_bits_lsr64(long long x, int n);

/* Bit rotates. Count masked by (W-1); rot-by-0 returns x unchanged. */
int       aether_bits_rotr32(int x, int n);
int       aether_bits_rotl32(int x, int n);
long long aether_bits_rotr64(long long x, int n);
long long aether_bits_rotl64(long long x, int n);

/* Count of set bits over the unsigned value. */
int aether_bits_popcount32(int x);
int aether_bits_popcount64(long long x);

/* Count leading zeros over the unsigned value, MSB-first. Defined as
 * the width (32 / 64) for a zero input. */
int aether_bits_clz32(int x);
int aether_bits_clz64(long long x);

/* Unsigned division / remainder. Returns 0 if the divisor is 0
 * (defined sentinel rather than a trap). */
int       aether_bits_udiv32(int a, int b);
int       aether_bits_urem32(int a, int b);
long long aether_bits_udiv64(long long a, long long b);
long long aether_bits_urem64(long long a, long long b);

/* Unsigned compare of two 64-bit values. Returns -1 / 0 / 1. */
int aether_bits_ucmp64(long long a, long long b);

#endif
