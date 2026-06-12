# aedis batch ask: mutable module globals + C macro-constant import (+ two integer-width diagnostics)

**Reporter:** aedis (the Redis → Aether port, sibling repo `aedis/`).
**Toolchain:** `ae 0.239.0`, aetherc at `8d60111`.
**Platform:** Linux x86_64 (port targets all Redis platforms eventually).

This is the next D-006-style batch from the aedis side: two small,
self-contained language features that together delete most of the
remaining *leaf-tier* C, plus two typechecker diagnostics for hazards
that shipped as real data corruption. It follows the format and
priority framing of `docs/redis-porting-language-gaps.md` (which is
baselined at v0.169 and could use a refresh — several of its P0s have
since shipped: `@c_import` header structs, C ABI scalar aliases,
`sizeof`/`offsetof`, `std.mem` `_sz` accessors, nested typed-fn-cast).

Status of the aedis leaf burndown for context: `adlist.c`, `pqsort.c`,
`syncio.c`, `sha1.c`, `sha256.c`, `strl.c`, `endianconv.c`,
`localtime.c` are deleted outright. What stops the next tranche of
deletions is no longer algorithm expressiveness — it is the two gaps
below.

---

## Ask 1: Mutable module-level globals

### Problem

Aether has no mutable state at module scope. Any C function whose whole
job is "algorithm + one persistent word of state" therefore keeps a
vestigial C file holding the state plus get/set accessors, with the
real logic already in `.ae`.

### Call-site census (aedis today)

| File | Residual C | State |
| --- | --- | --- |
| `rand.c` | 22 lines | one `static uint64_t rand48_x` + 2 accessors; the entire 48-bit LCG already lives in `rand.ae` |
| `mt19937-64.c` | whole file kept | the 312-word Mersenne Twister state array + `mti` index |
| `release.c` | 1 of its 4 residual lines | a 32-byte static buffer caching the formatted build id (formatting + cache policy already in `release.ae`) |

Same shape is guaranteed to recur in every module with a `static` cache
or PRNG: `dict.c`'s resize policy flags, `lru_clock` style counters,
etc.

### Proposed spelling

```aether
var rand48_x: uint64 = 0x1234ABCD330E      // module scope, mutable

redisLrand48() -> int32_t {
    rand48_x = (rand48_x * 0x5DEECE66D + 0xB) & 0xFFFFFFFFFFFF
    return (rand48_x >> 17) & 0x7FFFFFFF
}
```

### Required semantics

- Module-scope `var name: type = const-expr` emits a file-scope
  `static <ctype> name = <value>;` in the generated TU.
- Reads/writes from functions in the same module emit plain C
  identifier access. No accessor indirection, no TLS unless asked.
- Initializer restricted to constant expressions (no static
  constructors needed — matches C).
- Cross-module visibility can be punted: module-private (`static`) is
  enough for every aedis call site above. `pub var` / extern-linkage
  globals can come later if ever needed.
- Threading: plain non-atomic statics, same as the C they replace.
  Redis guards these by design (main-thread-only access); aedis does
  not ask for atomics here.

### What it deletes

`rand.c` and `mt19937-64.c` outright; the last buffer edge in
`release.c`. Every future "static + accessors" host file.

### Non-ask (related, bigger, P0 for the startup spine — flagging, not
spec'ing here)

Import of **C-owned** globals (`extern var server: ServerStruct
@c_import` → emits `server.maxidletime` against the header-defined
global). That is the gate on `timeout.c`'s remaining functions and the
whole `server.c` spine (aedis decision D-003). It deserves its own
design round; this batch intentionally contains only the small,
Aether-owned half.

---

## Ask 2: C macro-constant import

### Problem

Object-like C macros (`EAGAIN`, `EFD_NONBLOCK`, `REDIS_GIT_SHA1`,
`C_OK`, `AE_READABLE`, …) are invisible to Aether. The port currently
either (a) inlines the platform's numeric value with a comment — wrong
on the next platform — or (b) keeps a one-line C accessor per macro.

### Call-site census (aedis today)

| File | Today's workaround | Macros |
| --- | --- | --- |
| `syncio.ae` | values inlined, Linux-pinned | `EAGAIN`, `ETIMEDOUT`, `AE_READABLE`, `AE_WRITABLE` |
| `release.c` | 3 one-line C accessors | `REDIS_GIT_SHA1`, `REDIS_GIT_DIRTY`, `REDIS_BUILD_ID_RAW` (string macros, regenerated into `release.h` every build) |
| `eventnotifier.c` | function kept in C | `EFD_NONBLOCK`, `EFD_CLOEXEC`, `O_CLOEXEC`, `O_NONBLOCK` |
| `setcpuaffinity.c` | function kept in C | `CPU_SETSIZE` (the `CPU_SET` function-like family is out of scope here) |
| every future module | — | `C_OK`/`C_ERR`, `OBJ_*`, `LIST_HEAD`/`LIST_TAIL`, `LLONG_MAX`, errno family |

### Proposed spelling

```aether
extern const EAGAIN: int @c_import
extern const REDIS_GIT_SHA1: ptr @c_import     // string macro → const char *

if syncio_errno() == EAGAIN { ... }
```

### Required semantics

- The declaration teaches the typechecker a name and an Aether type.
  Generated C emits the macro name **verbatim** wherever the constant
  is used — no value is needed (or wanted) at Aether compile time; the
  including TU's headers are the source of truth, so per-platform
  values come out right by construction.
- No forward declaration is emitted for it (it is not a symbol).
- Works in expression and `if`/comparison contexts. Use in
  match-arm/constant-fold positions can be rejected in v1 (the macro's
  value is unknown to aetherc).
- Type is trusted as declared, same trust model as `extern` functions.
- Object-like macros only. Function-like macros (`CPU_SET(i, &set)`)
  are explicitly out of scope for this batch.

### What it deletes

The three `release.c` accessors (leaving only the Ask-1 buffer — i.e.
Ask 1 + Ask 2 together delete `release.c`); un-pins `syncio.ae` from
Linux; removes half of `eventnotifier.c`'s irreducibility (the
`#ifdef HAVE_EVENT_FD` branch selection still needs platform
conditionals — separate, future ask). Pre-empts a one-line C accessor
per constant across the entire command/type tier.

---

## Ask 3 (diagnostics): the two integer-width hazards that shipped corruption

Both already filed with full detail and repros; included in this batch
because they are small typechecker passes and they caused *silent data
corruption* in a shipped aedis build, found only by fuzzing:

- **aether-lang-org/aether#697 — int-shift sign-extension.**
  `get_byte(p, 4) << 24` lowers to a C `int` shift; bytes ≥ 0x80
  sign-extend on promotion into a `uint64` expression (and counts ≥ 32
  are outright UB — gcc masks `<< 48` to `<< 16`). Corrupted Redis
  listpacks: negative 32-bit-encoded values misread,
  `hashTypeConvertListpack` panicked with "Listpack corruption
  detected". Wish: promote-first semantics in 64-bit contexts, or
  warn on `<<` ≥ 24 of an int operand / error on ≥ 32.

- **aether-lang-org/aether#698 — silent narrowing via inference.**
  `parsed = 0` infers 32-bit int; a later `parsed = some_long`
  silently truncates. Made `lpFind` unable to match int-encoded hash
  fields beyond 32 bits (`HGET` returned nil for present fields).
  Wish: error/warn on narrowing assignment to an inferred-type local,
  or infer the widest assigned type across the block.

Fix commit on the aedis side for both: `ee52fce8e` ("listpack codec:
fix two silent data-corruption bugs").

---

## After this batch (not asked now, recorded for sequencing)

In aedis-impact order: C-owned global import (the D-003 spine gate,
see Ask 1 non-ask), varargs ABI for fixed-format `serverLog` shapes,
compile-time platform conditionals (`@c_ifdef`), exact fn-pointer
types in `@c_import` struct fields and public signatures, `long long`
ABI spelling (`mstime_t` clash with emitted `int64_t`), long double,
typedef-spelled `@c_import` + reserved-word field escaping
(`state`/`match`), `std.os` errno surface. Details for most of these
already live in `docs/redis-porting-language-gaps.md`.
