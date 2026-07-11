# Ask: Wrapping 64-bit Arithmetic Helpers

## Request

Add explicit 64-bit wrapping arithmetic helpers to `std.bits`:

```aether
bits.wrapping_add64(a: long, b: long) -> long
bits.wrapping_mul64(a: long, b: long) -> long
```

The helpers perform modulo-2^64 arithmetic and return the resulting 64-bit bit
pattern as `long`. They are the defined-overflow siblings of the unsigned
64-bit operations `std.bits` already exposes (`aether_bits_udiv64`,
`urem64`, `ucmp64`), and should be implemented the same way — thin externs over
`aether_bits_*` C functions that compute in `uint64_t`.

## Motivation

F3's fill/verify core is a linear-congruential generator whose every sector is
a run of these values written as little-endian `uint64`
(`src/libutils.c:219-221`, `fill_buffer_with_block` / `validate_buffer_with_block`):

```c
static inline uint64_t next_random_number(uint64_t random_number)
{
    return random_number * 4294967311ULL + 17;
}
```

This arithmetic **is** the on-disk validation format: `f3write` fills each
512-byte sector with the sequence, `f3read` regenerates it and compares. It has
to be bit-exact or the tool is wrong.

### What actually needs fixing (verified, not assumed)

The original version of this ask claimed a faithful port "either needs a C shim
or must change the file format." That is **not** true, and the correction
matters:

- Aether's native `long * long + long` already produces the **bit-identical**
  modulo-2^64 result as C's `uint64_t`. Verified empirically: seeding at 512 and
  iterating the LCG 20 times (well past 2^64 wrap) gives Aether
  `-8007105390423481696`, which is the same 64 bits as C's
  `10439638683286069920` — only the signed-vs-unsigned *display* differs. F3
  writes these as raw LE64 bytes (`bytes.set_le64`), so the interpretation is
  irrelevant; the bytes match.

So this is **not** a "the port is impossible without it" ask. The real reason to
add the helpers is **defined behavior**:

- Aether `long` lowers to signed `int64_t`, and signed overflow in C is
  **undefined behavior**. Aether does not emit `-fwrapv`. The native-multiply
  approach gets the right bytes at `-O2` today only by luck of 2's-complement
  wrap; an optimizer is free to miscompile it, and a UBSan build **traps** on it
  (confirmed: `runtime error: signed integer overflow: … cannot be represented
  in type 'long int'`). Aether ships a UBSan test path (`-fsanitize=undefined`),
  so a port that relies on signed-overflow wrap is a latent CI failure.

`wrapping_mul64` / `wrapping_add64` express the intent correctly — defined
modulo-2^64, computed in `uint64_t` on the C side — so the port is UBSan-clean
and optimizer-proof rather than depending on undefined behavior that happens to
work.

### Current port state (why this is stalled work, not hypothetical)

The in-progress port (`src/f3core/module.ae`) currently **fakes the format**:
`sector_payload` writes the offset then fills the rest of the sector with zeros
instead of the LCG sequence, and `validate_sector` checks for zeros to match.
That is precisely the "change the file format" outcome, adopted as a placeholder
because the RNG core isn't ported yet. These helpers unblock porting the real
`fill_buffer_with_block` / `validate_buffer_with_block`.

## Acceptance Criteria

- `std.bits` exports `wrapping_add64` and `wrapping_mul64`, each a thin wrapper
  over an `aether_bits_*` extern that computes in `uint64_t`.
- Behavior is modulo 2^64 on all supported targets, and is **defined** — a
  `-fsanitize=undefined` build of the emitted C does not trap.
- Tests cover overflow boundaries: values around `LONG_MAX`, `-1`
  (all-bits-set), multiplication by large constants (e.g. `4294967311`), and a
  multi-step LCG iteration asserted bit-for-bit against a `uint64_t` reference.
- The helpers can be used from pure Aether code without custom C sources.

## Notes for the implementer

- Mirror the existing `aether_bits_udiv64` shape in `std/bits/aether_bits.c` and
  its extern in `std/bits/module.ae`; add the friendly wrappers alongside
  `lsr64` etc.
- A one-line C body suffices: `return (int64_t)((uint64_t)a * (uint64_t)b);` and
  `return (int64_t)((uint64_t)a + (uint64_t)b);`.
