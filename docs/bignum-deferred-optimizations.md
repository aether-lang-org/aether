# std.bignum — deferred optimizations

The arbitrary-precision integer surface (`std/bignum/module.ae`, issue #739) is
**functionally complete and oracle-verified** — representation, byte I/O,
compare, add/sub/negate/abs/shift, multiply, divide/remainder/mod, mod_pow,
gcd, mod_inverse, is_probable_prime. Every operation has been fuzzed against
Python's arbitrary-precision `int` and is ASan-clean.

What remains is **performance**, not correctness. The current implementations
favour "obviously correct and easy to verify against an oracle" over speed.
The two highest-value fast paths — Montgomery-form `mod_pow` for odd moduli and
Karatsuba `multiply` for large operands — have since landed (commit 3437ae91)
as pure speed paths gated on parity/size, byte-identical to the slow path. This
is the punch-list of what is still deferred, in rough priority order for the
RSA/EC work that builds on this.

## 1. `mod_pow` — even-modulus Barrett reduction

Done: odd-modulus Montgomery reduction. `mod_pow` routes odd moduli (the RSA/DH
hot path) to `mod_pow_monty`, which uses `mont_mul` / `mont_mdash`
(`mdash` = -m⁻¹ mod 2³²) for square-and-multiply in Montgomery form. The
schoolbook path below handles even moduli and degenerate cases.

Deferred: the even-modulus fast path. Even moduli still fall through to textbook
square-and-multiply — each step does a full `multiply` followed by a full `mod`
(binary long division). For a k-bit modulus and k-bit exponent that's O(k)
modmuls, each O(k²) multiply + O(k²·?) division.

Bouncy Castle's `ModPow` (BigInteger.cs) also carries a sliding-window
exponentiation for the odd path (`ExpWindowThresholds`, `GetWindowList`) not yet
ported, and handles the even case with `ModPowBarrett` (`ReduceBarrett`,
precomputed `yu = 2^(64k)/m`).

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

## 3. `multiply` — Toom-Cook above a threshold

Done: Karatsuba. `multiply` switches to `le_karatsuba` when the smaller operand
has at least `karatsuba_threshold()` limbs; below that it uses the schoolbook
`Multiply(uint[],uint[],uint[])` (BC's, ported faithfully) as the base case and
fallback. O(n²) schoolbook for small operands, O(n^1.585) Karatsuba for large.

Deferred: Toom-Cook, for the very large operands where its lower asymptotic
exponent beats Karatsuba. Add it above a higher limb-count threshold.

## 4. `is_probable_prime` — random witnesses + Montgomery-form MR

Current: Miller-Rabin over a **fixed** set of the first 13 small primes as
witness bases. This is *deterministic* for all n < 3.317 × 10²⁴; beyond that it
is a strong probable-prime test with those fixed bases (not a random one).

Bouncy Castle's `RabinMillerTest` picks **random** bases (count scaled by bit
length and requested certainty) and works in Montgomery form. Montgomery
`mod_pow` now exists; once a vetted random source is available too, switch
large-n primality to random witnesses so the certainty argument is meaningful
past the deterministic range.
Also: small-prime trial division before MR (BC's `primeProducts` / `primeLists`)
to reject obvious composites cheaply.

## Why deferred

The doctrine for this port is *verify against an external oracle, never trust a
clever implementation you can't check*. The optimized algorithms (Montgomery,
Knuth division, Karatsuba, Toom-Cook) are exactly the kind of code where a
subtle index or carry bug hides behind correct-looking round-trips. Landing the
textbook forms first gave a trusted reference to differentially-test the fast
versions against as they get ported: Montgomery `mod_pow` and Karatsuba
`multiply` cleared that bar in commit 3437ae91; the items above are what remains.
Tracked as task #233.
