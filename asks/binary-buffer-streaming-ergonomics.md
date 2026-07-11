# Ask: Binary Buffer and Streaming Ergonomics

## Request

Two small additions to the binary I/O path, so a fixed-size block reader can
avoid a per-block copy and read little-endian integers straight out of a
streamed buffer:

1. `fs.pread_into(file, buf: bytes, len, offset) -> (int, string)` — read up to
   `len` bytes at `offset` directly into an existing `std.bytes` buffer,
   returning `(n, err)`. The point is to reuse one buffer across a validation
   loop instead of allocating a fresh string per block.
2. Little-endian readers on `std.bytes.cursor` (`read_le_u16/u32/u64`) to match
   the existing big-endian readers, since binary formats are frequently LE.

A documented recipe using existing APIs is an acceptable substitute for (2) if a
cursor extension isn't wanted.

## Motivation

F3 reads and writes repeated 512-byte sectors (`SECTOR_SIZE`,
`src/f3write.c` / `src/f3read.c`). The read path regenerates the LCG sequence and
compares it against each sector, in a tight loop over the whole device.

### What already works (verified against the in-progress port)

Most of what a block reader/writer needs is **already present**, so this ask is
deliberately narrow. In `src/f3core/module.ae`:

- **EOF vs. short read vs. I/O error is already distinguishable.** `read_h2w_file`
  branches on `fs.pread`'s `(payload, n, err)` return: `err != ""` is an I/O
  error, `n == 0` is EOF (break), `n != SECTOR_SIZE` is a short sector. No new
  API is needed for this — an earlier version of this ask listed it as a gap; it
  is not one.
- **Little-endian integer access on a buffer already exists.** `std.bytes` has
  both `set_le64` and `get_le64` (and the 16/32 variants). The port's write path
  uses `bytes.new` → `set_le64` → `finish`, which is clean. The port's read path
  currently hand-rolls an LE64 decode from a string byte-by-byte
  (`bytes_from_string_check`, assembling `b0 | (b1<<8) | …`); that is
  **unnecessary** — `bytes.copy_from_string` followed by `bytes.get_le64` does
  the same thing with the stdlib accessor.

### The actual gap

`fs.pread` returns a length-bearing **string**. To use `bytes.get_le64` on the
read-back, you must first copy that string into a `bytes` buffer
(`bytes.copy_from_string`) — a **copy per sector** in the hottest loop in the
tool. That copy is the only real inefficiency: everything else composes.

- `fs.pread_into(file, buf, len, offset)` removes it: read straight into a
  reusable `bytes` buffer, then `bytes.get_le64` in place. One buffer for the
  whole device.
- `std.bytes.cursor` is the natural "walk the sector's u64s" abstraction, but it
  only offers **big-endian** readers (`read_be_u16/u32/u64`). F3's format is
  little-endian, so the cursor can't be used as-is. LE readers would let the
  validation loop read successive u64s with bounds-checked cursor advances
  instead of manual index arithmetic.

Neither is a correctness blocker — the string + `copy_from_string` +
`get_le64` recipe is correct today (and binary-safe: `fs.pread` returns a
length-bearing string, so embedded NULs survive). This ask is purely about
removing the per-block copy and giving LE formats a first-class streaming
reader.

## Possible Shape

```aether
// One reusable buffer for the whole validation loop:
buf = bytes.new(512)
loop {
    n, err = fs.pread_into(file, buf, 512, offset)
    if err != "" { return io_error }
    if n == 0 { break }            // EOF
    if n != 512 { return short }   // short sector
    v0 = bytes.get_le64(buf, 0)    // no per-sector copy
    // …validate the sector's u64 sequence in place…
    offset = offset + 512
}
```

## Acceptance Criteria

- A pure-Aether fixed-size block reader can process repeated blocks **without a
  per-block string→bytes copy** (via `pread_into` into a reused buffer).
- Fixed-size reads continue to distinguish EOF (`n == 0`), short read
  (`0 < n < len`), and I/O error (`err != ""`) — this already holds for
  `fs.pread` and must hold for `pread_into` too.
- LE integer reads compose with `std.bytes` (`get_le16/32/64` on the buffer, and
  ideally `read_le_*` on a cursor).
- Tests include embedded NUL bytes (a sector whose payload contains `0x00`
  bytes must round-trip) and a repeated fixed-size block read/validate loop over
  several sectors, asserting the copy-free path yields the same result as the
  existing `pread` + `copy_from_string` recipe.

## Notes for the implementer

- `pread_into` is a thin sibling of the existing `fs.pread` extern in
  `std/fs/`, differing only in that it writes into a caller-owned
  `AetherBytes*` rather than allocating a new length-bearing string. Bounds:
  clamp `len` to the buffer capacity.
- The cursor LE readers mirror `bytes_cursor_read_be_u{16,32,64}` in
  `std/bytes/cursor/` with the byte order reversed; same EOF-returns-null,
  cursor-unchanged contract.
