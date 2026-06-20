# std.bignum — deferred optimizations

The arbitrary-precision integer surface (`std/bignum/module.ae`, issue #739) is
**functionally complete and oracle-verified** — representation, byte I/O,
compare, add/sub/negate/abs/shift, multiply, divide/remainder/mod, mod_pow,
gcd, mod_inverse, is_probable_prime. Every operation has been fuzzed against
Python's arbitrary-precision `int` and is ASan-clean.

What remains is **performance**, not correctness. The current implementations
favour "obviously correct and easy to verify against an oracle" over speed.
This is the punch-list of perfections we deliberately deferred, in rough
priority order for the RSA/EC work that builds on this.

## 1. `mod_pow` — Montgomery / Barrett reduction (highest value)

Current: textbook square-and-multiply. Each step does a full `multiply`
followed by a full `mod` (which is binary long division). For a k-bit modulus
and k-bit exponent that's O(k) modmuls, each O(k²) multiply + O(k²·?) division.

Bouncy Castle's `ModPow` (BigInteger.cs):
- odd modulus → `ModPowMonty` (Montgomery reduction, `MultiplyMonty` /
  `MontgomeryReduce`, with `GetMQuote` = -m⁻¹ mod 2³²), plus a sliding-window
  exponentiation (`ExpWindowThresholds`, `GetWindowList`).
- even modulus → `ModPowBarrett` (`ReduceBarrett`, precomputed `yu = 2^(64k)/m`).

This is the RSA hot path; it's the first thing to port when bignum perf matters.

## 2. `divide` / `remainder` — optimized long division

Current: bit-at-a-time binary long division (shift remainder left one bit,
compare, conditional subtract). O(bits × limbs).

Bouncy Castle's `Divide(uint[] x, uint[] y)` is a shift-and-subtract algorithm
that mutates the dividend in place to leave the remainder and returns the
quotient. A first port of it **infinite-looped** on a 5-limb / 2-limb case
(the `iCount` slice right-shifted to all-zeros and the unbounded
`while c[cStart]==0` / `while iCount[iCountStart]==0` advancers ran off the end
— see the muldiv PR discussion). The optimized version needs that index
bookkeeping understood and ported correctly, guarded by the same 414-case
Python oracle. Until then the binary version is the safe choice.

## 3. `multiply` — Karatsuba above a threshold

Current: schoolbook `Multiply(uint[],uint[],uint[])` (BC's, ported faithfully).
Fine for small operands; O(n²). BC switches to faster paths for large inputs.
Add Karatsuba (or Toom-Cook) above a limb-count threshold.

## 4. `is_probable_prime` — random witnesses + Montgomery-form MR

Current: Miller-Rabin over a **fixed** set of the first 13 small primes as
witness bases. This is *deterministic* for all n < 3.317 × 10²⁴; beyond that it
is a strong probable-prime test with those fixed bases (not a random one).

Bouncy Castle's `RabinMillerTest` picks **random** bases (count scaled by bit
length and requested certainty) and works in Montgomery form. Once a vetted
random source + Montgomery `mod_pow` exist, switch large-n primality to random
witnesses so the certainty argument is meaningful past the deterministic range.
Also: small-prime trial division before MR (BC's `primeProducts` / `primeLists`)
to reject obvious composites cheaply.

## Why deferred

The doctrine for this port is *verify against an external oracle, never trust a
clever implementation you can't check*. The optimized algorithms (Montgomery,
Knuth division, Karatsuba) are exactly the kind of code where a subtle index or
carry bug hides behind correct-looking round-trips. Landing the textbook forms
first gives a trusted reference to differentially-test the fast versions against
when they're ported. Tracked as task #233.
