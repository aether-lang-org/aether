# C3 stdlib vs Aether `std/` — a costed gap analysis

> **Purpose.** Enumerate what C3's standard library has that Aether's `std/`
> does not, decide which gaps are worth closing, and cost each as an Aether
> port. Companion to `c3.md` (the language survey) and `error-unification.md`
> (a feature that came out of it). Inventoried against `../c3c/lib/std` and
> the Aether tree; module lists and line counts are real, not recalled.

## 0. The method, and one thing this is NOT

This is a **survey**, not a transplant. The question "could we copy C3's
*generated C* into our missing pieces and work backwards to Aether?" was
considered and **rejected** — for reasons that are the whole point of the
project, not incidental:

- **It violates the committed constraint.** Committed code must be Aether;
  zero downstream C consumers (asserted repeatedly; `dtoa.c` is the single
  deliberate exception). Transplanted C is a C consumer, and "optionally
  port it back later" is how it becomes permanent.
- **C3's generated C is not a clean donor.** It is lowered against C3's
  runtime — its Optional/`fault` ABI (`fault f(T* out, …)`, the very rewrite
  Aether's error-unification design **rejected**), its allocator interface,
  its slice / `String` / `any` fat-pointer layouts, its reflection builtins.
  You would inherit C3's runtime, then stub it — more code than the port.
- **Working backwards from generated C is decompilation.** The names, type
  intent, and abstractions are gone; you would reconstruct `fault`/slice/
  generic intent from lowered switch tables. Porting from **C3 source** —
  which is readable and close to Aether's level — is strictly easier.
- **It defeats the ownership machinery.** The `_heap_` / `_seqheap_` /
  `_heapopt_` / uniform-heap tracking assumes *Aether-emitted* C. Hand
  C would bypass it, and every string it returns is a leak the macOS/valgrind
  gates would (correctly) flag.

So where a gap is worth closing, the plan is **port from C3 *source* →
Aether directly** — the same path that produced bitstruct and the
error-unification arc. C3's generated C is at most a *read-only reference*
for an algorithm's edge cases, never committed.

Each row below is rated: **PORT** (real gap, worth doing) · **THIN**
(partial today, worth rounding out) · **COVERED** (already have it, maybe
under a different name) · **SKIP** (C3 has it, we don't want it / it doesn't
fit Aether).

## 1. Where Aether is already ahead

Worth stating up front, because it reframes "gaps" as a two-way ledger.
Aether `std/` carries a large **application-level** surface C3's stdlib does
not ship at all:

| Area | Aether has | C3 stdlib |
| --- | --- | --- |
| HTTP | server, client, middleware, reverse-proxy, script-gateway, h2/TLS | client-ish primitives only (`net`) |
| ID types | uuid, ulid, ksuid, nanoid, tsid | `uuid` only |
| Crypto | argon2, scrypt, pbkdf2, hkdf, hmac, blake2, sha2/sha3/skein, ripemd\*, sm3, tiger, whirlpool, **ml-kem** (PQC), drbg | aes, chacha20, ed25519, argon2, keccak, rc4, dh — narrower, no PQC |
| Systems | ipc, capsicum/sandbox, casper, audit, snapshot, dl, signal | `os`, `threads` (broader OS surface, see §5) |
| Data | xml, json, zlib, lzf, bignum | json, xml, compression (deflate/gzip/zip) |

So the exercise is **not** "we're behind." It is "which of C3's *foundational*
modules would round out `std/`, given we already lead on the application tier."

## 2. High-value gaps — recommend PORT

### 2.1 `std.encoding` — base64 / hex / base32, as a real public module

**Gap.** base64 exists **only** as `cryptography.base64_*` — semantically
misplaced (it is encoding, not cryptography) and undiscoverable. Hex
encode/decode is scattered inside crypto/bignum internals. There is no
public `std.encoding` surface.

**C3 has:** `encoding/base64.c3` (259 lines), `hex.c3` (113), `base32.c3`
(276), plus `csv`, `ini`, `pem`, `codepage`.

**Recommendation: PORT** a `std.encoding` module exposing `base64.encode/decode`
(std + url-safe alphabets), `hex.encode/decode`, `base32`. **~5.5 rule:** the
base64 *implementation* already exists in `cryptography/aether_cryptography.c`
— this is mostly **re-surfacing** it under the right namespace as Aether
wrappers, plus a hex/base32 port. Small, high discoverability win. Fallible
decode returns `bytes!` (Phase-1 `T!` convention). **Cost: ~150–250 Aether
lines**, most of it re-export + hex.

### 2.2 `std.time` — a datetime/calendar layer over the raw clocks

**Gap.** We have raw timestamps only (`os.now_unix_ms`, `now_monotonic_ns`,
`now_local`). No civil calendar (year/month/day/hour), no formatting
(ISO-8601 / RFC-3339), no parsing, no duration arithmetic on calendar dates.

**C3 has:** `time/datetime.c3` (269), `time/time.c3` (202), `time/format.c3`,
`time/clock.c3`.

**Recommendation: PORT** a `std.time` with a `DateTime` value (civil fields +
epoch), `now()`, ISO-8601 format/parse, and duration math. This is a genuine
absence that application code hits constantly (logging timestamps, HTTP
`Date`, expiry math — some of which the http server already re-derives
inline). **Cost: ~300–450 Aether lines.** Note: Aether already has a
`Duration` language scalar (`TYPE_DURATION`, a signed-64-bit nanosecond
count) used by the `os` clock APIs — build calendar arithmetic on it rather
than introducing a second duration type.

### 2.3 `std.sort` — generic comparator sort

**Gap.** Sorting exists only as `collections.string_list_sort` /
`_sort_lex`. No generic sort over `intarr` / `longarr` / `floatarr` /
`list`, no `binary_search`.

**C3 has:** `sort/` (quicksort, mergesort, insertionsort, countingsort,
binarysearch — 869 lines total, heavily generic).

**Recommendation: PORT — scoped.** C3's version leans on generics Aether
lowers differently; do **not** transliterate the generic machinery. Instead
add `sort(arr, cmp)` + `binary_search` over the concrete array types we
already ship (`intarr`/`longarr`/`floatarr`/`list`), with a comparator
closure (closures landed this year). The `string_list_sort` C already
demonstrates the stable-sort-with-comparator shape to mirror. **Cost:
~200–350 Aether lines** across the array types; a single reusable C
merge-sort core behind them if hand-C is warranted (it is a runtime
primitive, like the existing seq/list C, not a downstream consumer — check
against the no-C rule before committing).

## 3. Medium-value — recommend THIN → round out

### 3.1 Collections breadth

We ship list, map, set (C-backed), intarr/longarr/floatarr, pqueue,
stringlist, stringseq, bits. C3 additionally has **deque**, **ringbuffer**,
**bitset** (as a first-class value), **linkedlist**, **orderedmap/set**,
**range**. Most are niche. The two with real pull:

- **`deque` / `ringbuffer`** — genuinely useful (work queues, sliding
  windows) and not expressible cleanly today. **THIN → PORT if demand:**
  ~150–250 Aether lines each, or one ringbuffer that backs both.
- **first-class `bitset`** — we have `std.bits` (bit ops) and a C
  `aether_bits.c`; a value-type bitset is a thin wrapper. **THIN**, low
  priority.

`linkedlist` / `orderedmap` / `range` / `anylist`: **SKIP** — either niche or
lean on C3 generics/`any` that don't fit Aether's model.

### 3.2 `math` breadth

Our `std.math` covers abs/min/max/clamp/sqrt/pow/trig/log/exp/round/random +
constants. C3 adds **vectors / matrices / quaternions / complex /
distributions / easing**. These are **domain** libraries (graphics, stats),
not core stdlib. **THIN/SKIP:** port `vector`/`matrix` only if a consumer
appears; leave the rest. A cheap immediate win: our `math_random_*` is a
plain LCG-style seed API — C3's `random` offers named PRNGs; if crypto-grade
randomness is wanted, we already have `cryptography.drbg`, so this is
**COVERED** for the security case.

### 3.3 Hashing (non-crypto)

C3 ships many **non-cryptographic** hashes (fnv, murmur, wyhash, metro,
siphash, crc32/64, adler32). We have crc32/adler inside `zlib`, and the crypto
hashes. A public `std.hash` with fnv/murmur/siphash (for hashmap seeding,
checksums) is a **THIN** gap — **~150 lines**, only if a consumer wants it.
Not urgent: the C hashmap already has an internal hash.

## 4. Encoding formats — case-by-case

- **`csv`** (C3: 94 lines) — **PORT** candidate, small and broadly useful;
  fallible parse → `T!`. ~120 Aether lines.
- **`ini`** (300 lines) — **THIN**, useful for config; but we already have
  `std.config` — check overlap first, likely re-shape rather than new.
- **`pem`** — **COVERED-ish**: relevant only with the crypto stack, which is
  already deep; port only if a TLS/key path needs it.
- **`toml`/`yaml`** — C3 doesn't ship these either; out of scope.

## 5. Deliberately SKIP

- **`os` breadth (48 files) / `libc` / `_nolibc` / `_compiler_rt`** — C3's
  are its platform/runtime substrate. Aether's `os`/`fs`/`signal`/`ipc`/
  `dl` already cover the surface application code needs, and Aether's
  compilation model + sandbox story differ. Not a gap; a different design.
- **`threads` (14 files)** — Aether's concurrency is **actors**
  (`std.actors` + scheduler), a deliberate model choice. Raw threads/mutexes
  are intentionally not the surface. **SKIP.**
- **Generics-heavy modules** (`anylist`, `object`, `interfacelist`,
  `list_common`, `result`, `maybe`) — these lean on C3's generics and `any`.
  Aether's equivalents are `T?` (#340) and `T!` (the error-unification arc),
  already shipped and idiomatic. **COVERED** by different, better-fitting
  machinery.

## 6. Recommended order (if we do any of it)

Ranked by value ÷ cost, each an independent PR in the established style
(costed, tested, leak-gated):

1. **`std.encoding`** (base64 re-surface + hex + base32) — highest
   value/cost; mostly re-export of existing C. §2.1.
2. **`std.time`** (datetime/format/parse) — genuine, frequently-hit absence.
   §2.2.
3. **`std.sort`** (generic comparator sort + binary_search over our array
   types) — closes an obvious hole; scope to concrete types, not generics.
   §2.3.
4. **`csv`** under `std.encoding` — small, useful. §4.
5. **`deque`/`ringbuffer`** in collections — if a consumer appears. §3.1.

Everything below that is demand-driven. Nothing here is required; the point
of the survey is that the *high-value* set is small (encoding, time, sort),
cheap, and portable **from C3 source, staying pure Aether** — no C
transplant, no ABI import, consistent with every constraint the project
already holds.

## 7. What this survey deliberately did not do

- No line-by-line C3 API transcription — that is the port's job, per module,
  once picked.
- No claim that C3's *design* for a module is the one to copy: e.g. C3's sort
  is generic-first; Aether's should be concrete-types-first. Port the
  *capability*, re-derive the *shape* for Aether — the same discipline
  `error-unification.md` used to land on "not C3's design."
