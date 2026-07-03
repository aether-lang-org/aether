# Redis Porting: Remaining Aether Needs

*Baseline: Aether `origin/main` at `v0.169.0-2-gc3ab9a8`. This document is
for Aether maintainers planning language/runtime work to accelerate the in-situ
Redis-to-Aether migration in sibling repo `aedis/`. It intentionally excludes
features already present in `0.169`.*

> **Status update (last reviewed at v0.347.0).** This is a historical wishlist written
> at v0.169.0; the original analysis below is preserved verbatim. Several of
> the P0/P1/P2 items have since landed in the compiler and stdlib. Each
> delivered item is annotated inline with a `✅ DELIVERED` marker and the
> implementing evidence — do not read the unmarked gap text as still-missing.
> Quick index of what has shipped:
>
> - P0 Header-Defined C Struct Interop — **DELIVERED** (`extern struct … @c_import`)
> - P0 Typed And Qualified C Pointers — **DELIVERED** (`extern type`, `*T`, `const ptr`, `cstring`/`cstring_const`)
> - P0 C ABI Scalar Aliases — **DELIVERED** (`size_t`/`ssize_t`/`uint32_t`/… builtins)
> - P1 Long Double — **DELIVERED** (`longdouble`, #749)
> - P1 Function Pointer Fields And Parameters — **DELIVERED** (#749 / #750)
> - P1 C Constants And Macros — **DELIVERED** (`extern const NAME: type @c_import`, #702)
> - P2 `std.mem` Bulk And Endian Helpers — **DELIVERED** (`mem.copy/move/compare/set`, LE/BE 16/32/64)
>
> Still open at v0.347.0: P2 SDS And Length-Aware String Boundary, and the
> P3/P4 build-integration and ergonomics items.

## Current Port Shape

The Redis port is dependency-first: small, widely used helpers move before
large subsystems. As of this note:

- `42` Aether source modules exist under `aedis/src/*.ae`.
- `42` generated companions exist under `aedis/src/*_ae.c`.
- `123` non-generated C translation units remain under `aedis/src`.
- Aether source is about `4.1k` lines.
- Generated Aether C is about `13.3k` lines.
- Remaining non-generated C is about `175k` lines.

Recent migration work has moved byte traversal, zipmap/ziplist codecs, CRCs,
hashing helpers, list/vector/intset helpers, numeric parsing/formatting
helpers, ACL byte helpers, GCRA math, sparkline rendering, and several small
platform helpers. The next layer is constrained less by algorithms and more by
C ABI fidelity: Redis is dense with existing structs, typed pointers, macros,
callbacks, `size_t`-family types, and hand-managed buffers.

## Already Covered In Aether 0.169

Do not treat these as outstanding requirements:

- `float` now consistently lowers to C `double`.
- `ptr + int`, `int + ptr`, and `ptr - int` preserve `ptr` type.
- `std.mem` provides raw pointer loads/stores, pointer slots, integer slots,
  float bit reinterpretation, and low-level function-pointer call helpers.
- Typed C function-pointer locals and casts exist via `fn(T, ...) -> R`.
- `extern struct` exists, including imports, bitfields, flexible-array tails,
  and field access through pointer overlays.
- `uint64` and wide integer literals exist.
- `std.strbuilder` exists, including formatted append and binary-safe finish.
- `std.lzf` exists.
- Heap string ownership has had several important fixes through `0.169`.

The remaining asks below are narrower than the older wishlist.

## P0: Header-Defined C Struct Interop

> **✅ DELIVERED (as of v0.303.0):** the second spelling proposed below —
> `extern struct client @c_import { argc: int; argv: ptr }` — is implemented.
> The `@c_import` attribute marks a struct whose layout is *imported from a C
> header*: Aether typechecks field access and `*client` overlays against the
> declared fields, but codegen emits **no** `typedef struct client { … }
> client;` and no forward typedef, so it cannot collide with the header's.
> Parser: `compiler/parser/parser.c:4033-4083` (attribute → annotation
> `extern_c_import`, mutually exclusive with `@packed`). Codegen suppresses
> the body/typedef at `compiler/codegen/codegen_func.c:1804-1806`, and emits the
> portable `struct Name*` pointer form at `compiler/codegen/codegen.c:1761`
> (`aether_is_c_import_struct`).
> *(Original gap analysis preserved below.)*

### Problem

`extern struct` in `0.169` describes and emits an Aether-owned C layout:

```aether
extern struct Foo {
    field: int
}
```

The generated C contains a typedef for `Foo`. That is useful when Aether and
nearby glue jointly own the layout. It is not enough for Redis core structs,
which are already defined in Redis headers. If Aether emits another typedef
for `client`, `robj`, `dict`, `dictEntry`, `connection`, `rio`, `listpack`,
or module-facing structs, it conflicts with the included Redis headers.

Today the port often falls back to C accessor shims or keeps code in C.

### Requirement

Add a way to declare a struct layout that is **imported from C headers** rather
than emitted by Aether.

Possible spelling:

```aether
extern import struct client {
    argc: int
    argv: ptr
}
```

or:

```aether
extern struct client @c_import {
    argc: int
    argv: ptr
}
```

Required semantics:

- Aether typechecks field access and pointer overlays using the declared fields.
- Generated C does **not** emit `typedef struct client { ... } client;`.
- Generated C may emit only a forward declaration if needed and safe:
  `typedef struct client client;` must also be suppressible when Redis headers
  already provide it.
- The intended C header remains the source of truth for size, layout, padding,
  and typedef spelling.
- Field reads/writes emit ordinary C member access against the header-defined
  type, e.g. `c->argc`.
- Imported declarations must work across Aether module imports, as `extern
  struct` does now.

### Acceptance Tests

- A C header defines `typedef struct client { int argc; void *argv; } client;`.
- An Aether file includes or is compiled beside that header, declares imported
  `client`, casts a `ptr` to `*client`, reads and writes fields, and compiles
  with no duplicate typedef.
- A second Aether module imports the declaration and uses the fields.
- A negative test proves a missing field still fails at Aether typecheck time.

### Redis Impact

This is the largest unlock for moving command bodies and mixed C/Aether core
logic without writing one accessor function per field.

Primary targets:

- `client`, `robj`, `redisDb`
- `dict`, `dictEntry`, `dictType`
- `connection`, `ConnectionType`
- `rio`
- `list`, `listNode`
- `quicklist`, `listpack`, `rax`
- module API context structs

## P0: Typed And Qualified C Pointers

> **✅ DELIVERED (as of v0.303.0):** opaque header-defined pointee types and
> qualified C pointers are implemented. `extern type Name` declares an opaque,
> header-defined C type (emits a forward `typedef struct Name Name;`, no field
> access); `*Name` is its typed pointer, and `const ptr` (→ `const void*`),
> `const *T` (→ `const T*`), `cstring` (→ `char*`) and `cstring_const`
> (→ `const char*`) cover the qualified surface. Parser: `extern type`
> at `compiler/parser/parser.c:4007-4021` (annotation `extern_opaque`).
> Documented in `docs/language-reference.md:1879` ("Typed and qualified C
> pointers"). Regression test:
> `tests/regression/test_c_typed_pointers.ae` exercises the doc's acceptance
> list (`uint64_t h(const void *, size_t)`, `int f(const char *)`, opaque
> `*handle` param, no conflicting-declaration warnings).
> *(Original gap analysis preserved below.)*

### Problem

Aether `ptr` emits as `void *`. That is too lossy for Redis APIs. It causes
prototype conflicts when Aether declares an existing C function whose header
uses `const void *`, `const char *`, `char *`, or a named struct pointer.

Concrete example from the Redis port: directly declaring
`dictGenHashFunction(const void *, size_t)` is not expressible. Declaring it
with `ptr` emits `void *`, which conflicts with Redis' header prototype.

### Requirement

Add C ABI pointer types with both named pointees and qualifiers.

Minimum useful surface:

```aether
extern type client
extern type dictEntry

extern addReplyLongLong(c: *client, value: long)
extern dictGetKey(de: *dictEntry) -> ptr
extern memcmp(a: const ptr, b: const ptr, n: size_t) -> int
extern strlen(s: cstring_const) -> size_t
```

The exact syntax can differ, but Aether needs to express these emitted C
types:

- `void *`
- `const void *`
- `char *`
- `const char *`
- `T *` for opaque/header-defined `T`
- `const T *`

Required semantics:

- Extern declarations emit prototypes that match C headers exactly.
- Aether can still pass ordinary `ptr` where C conversion is safe, but emitted
  prototypes must not drift from the header.
- Opaque pointer aliases do not require field access.
- Qualified pointers should prevent accidental writes where Aether can see the
  constness, or at minimum preserve ABI and diagnostics in generated C.

### Acceptance Tests

- Declare and call a C function with exact prototype
  `uint64_t h(const void *key, size_t len)`.
- Declare and call `int f(const char *s)`.
- Declare opaque `extern type client`; call `void g(client *c)`.
- Compile alongside a header with the same prototypes and no conflicting
  declaration warnings.

### Redis Impact

This removes many wrapper functions and lets Aether call Redis internals
directly. It is prerequisite for larger ports of `dict.c`, ACL helpers,
command handlers, networking, module APIs, and storage code.

## P0: C ABI Scalar Aliases

> **✅ DELIVERED (as of v0.303.0):** the built-in-alias spelling proposed below
> (`size_t n = 0`, `uint32_t flags = 0`) is implemented. The fixed-width and
> platform C type names are recognised as type keywords and mapped to Aether's
> integer types, which emit the matching exact C spelling. Mappings in the
> `c_abi_alias_kind()` table at `compiler/parser/parser.c:160-181`, including
> `uint8_t`→`TYPE_UINT8`,
> `uint16_t`→`TYPE_UINT16`, `uint32_t`→`TYPE_UINT32`, `int64_t`/`uint64_t`→
> `TYPE_INT64`/`TYPE_UINT64`, `intptr_t`/`uintptr_t`→`TYPE_INT64`/`TYPE_UINT64`,
> `size_t`→`TYPE_UINT64`, `ssize_t`/`off_t`/`time_t`→`TYPE_INT64`. Used directly
> by the `test_c_typed_pointers.ae` externs (`len: size_t`, `-> uint64_t`).
> *(Original gap analysis preserved below.)*

### Problem

Redis uses platform and fixed-width C types throughout. Aether has `int`,
`long`, `uint64`, `byte`, and `float`/C `double`, but C signatures often need
exact spellings:

- `size_t`
- `ssize_t`
- `uint8_t`, `int8_t`
- `uint16_t`, `int16_t`
- `uint32_t`, `int32_t`
- `int64_t`, `uint64_t`
- `uintptr_t`, `intptr_t`
- `time_t`
- `pid_t`
- `mode_t`
- `off_t`

Using a larger or merely equivalent Aether type can still produce conflicting
prototypes, wrong varargs ABI, or unclear generated C.

### Requirement

Add ABI scalar aliases that emit the exact C type name where requested.

Possible spelling:

```aether
extern ctype size_t
extern ctype ssize_t
extern ctype uint32_t

extern fread(ptr: ptr, size: size_t, nmemb: size_t, stream: ptr) -> size_t
```

or built-in aliases:

```aether
size_t n = 0
uint32_t flags = 0
```

Required semantics:

- The generated C prototype uses the exact alias spelling.
- Arithmetic and comparisons work with sensible promotions.
- Casts between ABI aliases and existing integer types are explicit when
  narrowing.
- Varargs calls preserve correct ABI width.

### Acceptance Tests

- Aether extern matching `size_t strlen(const char *)` compiles beside
  `<string.h>`.
- `uint32_t` fields in imported structs round-trip without widening warnings.
- `uintptr_t` can hold `ptr` round-trips without truncation.
- `ssize_t` negative return paths typecheck.

### Redis Impact

This unblocks direct prototypes for allocators, SDS, networking, filesystem,
RDB/AOF, process helpers, and platform-specific code.

## P1: Long Double

> **✅ DELIVERED (as of v0.303.0):** the `longdouble` ABI type is implemented
> (#749). The `longdouble` type name parses to `TYPE_LONGDOUBLE`
> (`compiler/parser/parser.c:583-588`, declared in `compiler/ast.h:238`) and lowers
> to C `long double` in every codegen position (`compiler/codegen/codegen.c:221`,
> `:1451`, `:1710`, `:1902`). It works in extern params/returns, locals, and
> arithmetic, so direct libc externs such as `strtold` are expressible.
> *(Original gap analysis preserved below.)*

### Problem

Aether `float` now maps to C `double`, which is good for many Redis paths.
Redis still uses `long double` for exact command semantics in places such as
numeric parsing, `INCRBYFLOAT`, score conversion, and timeout handling.

### Requirement

Add a `long double` ABI type.

Possible spelling:

```aether
longdouble x = 0.0
extern strtold(s: const char *, end: *char *) -> longdouble
```

Required semantics:

- Emits C `long double`.
- Supports arithmetic and comparison.
- Supports casts to/from integer and `float`/C `double`.
- Works in extern parameters and returns.
- Supports formatting/parsing helper APIs, or at least allows direct libc
  externs such as `strtold`.

### Acceptance Tests

- Aether function returns `long double`; C caller sees exact `long double`.
- Aether calls a C function taking and returning `long double`.
- Basic arithmetic and comparisons compile and pass runtime tests.

### Redis Impact

This targets `util.c`, `t_string.c`, sorted-set score conversion, and timeout
parsing. It is not the same as the old "need double" request; C `double` is
largely covered by Aether `float` as of `0.169`.

## P1: Function Pointer Fields And Parameters

> **✅ DELIVERED (as of v0.303.0):** typed function-pointer struct **fields**
> (#749 Ask A) and function-pointer **parameters** (#750) are both implemented,
> with the `fn(T1, T2) -> R` spelling used in the requirement below. A struct
> field typed `fn(int) -> int` emits the C field `int (*name)(int)` and is
> callable as `o.field(args)` (value) or `p.field(args)` → `p->field(args)`
> (pointer-to-struct), and is assignable through a pointer — this is the
> `dictType` vtable shape. A `fn(...) -> R` parameter lowers to the exact C
> function-pointer parameter type and a call through it is a real indirect call.
> Regression tests: `tests/regression/test_fn_pointer_struct_fields.ae`
> (value + pointer dispatch, field assignment) and
> `tests/regression/test_fn_pointer_params.ae` (void/int callbacks, two
> callback params). Aether functions are turned into C-ABI function pointers
> for these via the `<fn> as fn(...) -> R` cast (and exported with
> `@c_callback`). *(Original gap analysis preserved below.)*

### Problem

Aether `0.169` supports typed function-pointer locals and casts:

```aether
fp = raw as fn(int, int) -> int
result = fp(1, 2)
```

Redis also stores callbacks in struct fields and passes callbacks through
function parameters. Examples include `dictType`, connection vtables, module
APIs, qsort-like comparators, and command dispatch metadata.

It is not enough to call a local function pointer if Aether cannot declare and
typecheck fields like:

```c
int (*keyCompare)(dictCmpCache *, const void *, const void *);
void (*free)(void *);
```

### Requirement

Extend typed function-pointer support to:

- imported/header-defined struct fields
- Aether-owned extern structs
- function parameters
- function return values where C APIs return callbacks

Possible spelling:

```aether
extern import struct dictType {
    hashFunction: fn(const ptr) -> uint64_t
    keyCompare: fn(*dictCmpCache, const ptr, const ptr) -> int
}

call_compare(t: *dictType, cache: *dictCmpCache, a: const ptr, b: const ptr) -> int {
    return t.keyCompare(cache, a, b)
}
```

Required semantics:

- Field access preserves the callback signature.
- Calling a function pointer field emits an inline C cast or typedef-backed
  call with the exact signature.
- Null checks against function pointer fields work.
- Assigning Aether `@c_callback` functions into compatible fields is either
  supported or rejected with a clear diagnostic.

### Acceptance Tests

- C header defines a struct with a function-pointer field.
- Aether imports the struct, checks field against `null`, and calls it.
- Aether passes a function pointer as a parameter to a C function.
- Mismatched argument count/type is rejected.

### Redis Impact

This unlocks callback-heavy structures without switch-based or per-signature
C trampoline code:

- `dictType`
- connection type tables
- module callbacks
- quicklist/listpack comparator paths
- event-loop callbacks

## P1: C Constants And Macros

> **✅ DELIVERED (as of v0.303.0):** the "minimum viable path" — importing an
> object-like C macro as a typed constant — is implemented (#702), with the
> spelling `extern const NAME: type @c_import` (e.g.
> `extern const EAGAIN: int @c_import`,
> `extern const REDIS_GIT_SHA1: ptr @c_import`). The declaration teaches the
> typechecker a name + Aether type (trusted as declared, like an extern fn);
> codegen emits the macro name **verbatim** at each use site and emits nothing
> for the declaration itself, so per-platform macro values come out right by
> construction (the including TU's headers are the source of truth). Object-like
> macros only; function-like macros stay wrapped in a C extern. Parser:
> `compiler/parser/parser.c:4110-4152` (annotation `c_import_const`, `@c_import`
> required). Documented in `docs/language-reference.md:1915`. The header-driven
> codegen tool described under "Better path" is not part of this; constants
> are imported by name, not generated. *(Original gap analysis preserved below.)*

### Problem

Redis logic relies on constants and macro expressions from headers:

- object encodings and type tags
- command flags
- ACL categories
- errno/platform constants
- struct offset/size macros
- byte-order macros

Today these are manually duplicated in Aether or kept behind C wrappers.
Manual duplication is error-prone and becomes stale when Redis changes.

### Requirement

Provide a supported way to import or generate Aether constants from C headers.

Minimum viable path:

```aether
extern const OBJ_STRING: int @c("OBJ_STRING")
extern const ACL_CATEGORY_READ: uint64_t @c("ACL_CATEGORY_READ")
```

This could lower by emitting C constants into generated code rather than
knowing the value in the Aether compiler.

Better path:

- A tool reads selected C headers and emits an Aether module of constants.
- Macro expressions that are integer constant expressions are supported.
- Unsupported macro functions are reported clearly.

Required semantics:

- Constants can be used in Aether conditions, assignments, and calls.
- Imported constants preserve type width.
- If compile-time value is not known to Aether, codegen still emits valid C.

### Acceptance Tests

- Import constants from a test header and use them in branches.
- Use a `uint64_t` flag constant without truncation.
- Attempt to import an unsupported function-like macro and get a clear error.

### Redis Impact

This reduces duplicated constants in ACLs, object/type code, networking,
RDB/AOF, module APIs, and platform conditionals.

## P2: `std.mem` Bulk And Endian Helpers

> **✅ DELIVERED (as of v0.303.0):** `std.mem` now exposes the bulk and endian
> surface proposed below. Bulk: `mem.copy` (memcpy), `mem.move` (memmove),
> `mem.compare` (memcmp), `mem.set` (memset). Endian: `get_/set_u16_le`,
> `get_/set_u16_be`, `get_/set_u32_le`, `get_/set_u32_be`, `get_/set_u64_le`,
> `get_/set_u64_be` — full LE/BE 16/32/64. All defined and exported in
> `std/mem/module.ae` (wrappers at lines ~318-334 over the `aether_mem_*`
> externs ~251-276), backed by libc/intrinsics in `std/mem/aether_mem.c`.
> The length parameter is `long` here rather than the `size_t` shown in the
> sketch. *(Original gap analysis preserved below.)*
>
> **Width-audit (#868):** the raw-offset idiom (`mem.get_long(p, OFF)`) chooses
> its read width by accessor *name*, not by the field's type, so reading a
> 4-byte field with `get_long` silently pulls in adjacent bytes (a real port
> read a `uint32` `slen` with `get_long` and crashed on a ~94 TB allocation).
> The width-exact accessors needed to do it right already exist
> (`get_uint32` / `get_u32_le` / …); `aetherc --audit-mem <file>` now lists
> every `mem.get_*`/`mem.set_*` call with the byte width its name implies, so
> each access can be checked against the C layout in one pass.

### Problem

`std.mem` now gives enough byte and slot access for many ports. Redis still has
many patterns where the idiomatic operation is bulk memory movement or endian
load/store:

- `memcpy`, `memmove`, `memcmp`, `memset`
- load/store little-endian or big-endian 16/32/64-bit values
- compare byte ranges
- pointer + offset materialisation for C handoff

The port currently uses small C shims or explicit byte loops in Aether. Byte
loops are correct but noisy and sometimes much slower than the intended C
primitive.

### Requirement

Extend `std.mem` with bulk operations and endian helpers over caller-owned
memory.

Suggested surface:

```aether
mem.copy(dst: ptr, src: const ptr, n: size_t) -> ptr      // memcpy
mem.move(dst: ptr, src: const ptr, n: size_t) -> ptr      // memmove
mem.compare(a: const ptr, b: const ptr, n: size_t) -> int // memcmp
mem.set(dst: ptr, value: int, n: size_t) -> ptr           // memset

mem.get_u16_le(p: const ptr, off: int) -> uint16_t
mem.set_u16_le(p: ptr, off: int, v: uint16_t)
mem.get_u32_le(p: const ptr, off: int) -> uint32_t
mem.set_u32_le(p: ptr, off: int, v: uint32_t)
mem.get_u64_le(p: const ptr, off: int) -> uint64_t
mem.set_u64_le(p: ptr, off: int, v: uint64_t)
// and big-endian variants
```

Required semantics:

- Implemented with C intrinsics/libc where appropriate.
- Safe for unaligned memory.
- Endian helpers have byte-exact tests on little- and big-endian simulation
  or explicit byte expectations.
- Length type should be ABI-correct (`size_t`) once available.

### Acceptance Tests

- Copy/move overlapping buffers correctly.
- Compare equal and unequal buffers.
- Load/store exact byte patterns for LE/BE 16/32/64.
- Compile without custom project shims.

### Redis Impact

This helps ziplist/listpack/zipmap, RDB/AOF binary formats, CRC/hash inputs,
SDS, networking buffers, and compression codecs.

## P2: SDS And Length-Aware String Boundary

### Problem

Redis `sds` is a `char *` with length/capacity metadata before the pointer.
Aether `string` is C-string shaped at the surface. Redis values may contain
embedded NULs, and Redis code often already knows lengths. Treating SDS as a
plain string risks truncation, repeated `strlen`, or ownership mistakes.

### Requirement

Provide either a Redis-local Aether support module or general stdlib patterns
for length-aware borrowed/owned string boundaries.

Needed operations:

```aether
sds.len(s: sds) -> size_t
sds.view_ptr(s: sds) -> const ptr
sds.to_owned_string(s: sds) -> string       // copy, NUL-safe where possible
sds.from_bytes(p: const ptr, n: size_t) -> sds
sds.free(s: sds)
```

For general Aether:

- A string/bytes view type carrying `(ptr, len)` without implying ownership.
- Clear ownership annotations for returned malloc/SDS/AetherString buffers.
- Easy conversion from `std.strbuilder.finish_with_length` to C-owned byte
  buffers without losing the length.

### Acceptance Tests

- Embedded-NUL SDS round-trips through a length-aware API.
- Borrowed view does not free underlying Redis memory.
- Owned conversion is freed by the right allocator.

### Redis Impact

This is required before large command implementations can move safely. Redis
command bodies constantly touch SDS-backed keys and values.

## P3: Lean Embedded Generated-C Mode

### Problem

The current generated `_ae.c` includes a full Aether prelude per source file.
For embedded helper ports, this creates three problems:

- generated C is much larger than the migrated logic
- every including C translation unit needs warning suppression for unused or
  deprecated prelude functions
- one C translation unit cannot include two independently generated `_ae.c`
  files because runtime/prelude symbols collide

The Redis port works around this by folding unrelated helper batches into a
single `.ae` file per C translation unit.

### Requirement

Add a codegen mode for embedded helper output.

Possible CLI:

```sh
aetherc --emit-c-helper input.ae output.c
```

Desired behaviour:

- Emit only the functions/constants requested from the Aether source.
- Do not emit duplicate runtime/prelude definitions already supplied by a
  shared support object.
- Put helper-private generated functions behind a unique prefix or `static`
  names safe for inclusion.
- Optionally emit a companion header of public prototypes.
- Support multiple generated helper files included or linked into one C
  translation unit without symbol collision.

### Acceptance Tests

- Two generated helper files can be included in one C file and compile.
- The same helpers can alternatively be compiled as separate objects and link.
- No deprecated `malloc/free` prelude warnings appear when the helper does not
  use heap strings.

### Redis Impact

This lowers porting friction and keeps diffs reviewable. It also avoids the
artificial "one `.ae` per C translation unit" constraint.

## P3: Better Header/Build Integration

### Problem

The Redis port currently hand-wires generated files and any required runtime
objects into Redis' makefiles. Importing `std.mem` required adding
`aether_mem.o` manually. Future stdlib modules may need additional runtime C
objects.

### Requirement

Provide machine-readable dependency output for generated C.

Possible CLI:

```sh
aetherc --emit-c-deps foo.ae
```

or makefile fragment output:

```make
foo_ae.o: ...
AETHER_RUNTIME_OBJS += .../std/mem/aether_mem.o
```

Required semantics:

- List generated C output dependencies.
- List required stdlib/runtime C objects.
- List include paths if needed.
- Stable enough for non-Aether build systems.

### Redis Impact

This prevents build-system drift as the port uses more stdlib modules.

## P4: Ergonomics And Diagnostics

These are not primary blockers, but they would reduce time spent debugging:

- Diagnostics for C prototype mismatch when an extern shadows a header symbol.
- A way to request exact emitted C type for any Aether declaration.
- Better warnings when `extern struct` would emit a typedef colliding with an
  included header type.
- Escaped identifiers for names that collide with Aether keywords or C
  reserved words in imported APIs.
- `errno` access helpers with thread-local correctness:

  ```aether
  errno.get() -> int
  errno.set(value: int)
  errno.message(value: int) -> string
  ```

## Priority Summary

P0, in order:

1. Header-defined C struct interop without duplicate typedef emission.
2. Typed and qualified C pointers.
3. C ABI scalar aliases.

P1:

4. `long double`.
5. Function pointer fields/parameters.
6. C constants/macros.

P2:

7. `std.mem` bulk/endian helpers.
8. SDS and length-aware string boundary support.

P3/P4:

9. Lean embedded generated-C mode.
10. Build dependency output and diagnostics/ergonomics.

## Concrete Redis Targets Unblocked

With P0 complete:

- `dict.c` can move more hash/equality/table logic without prototype conflicts.
- `connection.c`, `socket.c`, and `unix.c` can use typed connection structs.
- Command bodies can accept `client *` and `robj *` directly.
- `rio.c`, `rdb.c`, and AOF helpers can expose exact callback/state types.

With P1 complete:

- `util.c` long-double parsing/formatting paths become viable.
- `t_string.c` float command semantics can move.
- `dictType` and connection vtables can be represented directly.
- ACL/object/module constants stop being manually mirrored.

With P2 complete:

- listpack/ziplist/zipmap/RDB binary walking becomes smaller and faster.
- SDS-heavy command paths can move without NUL/ownership hazards.

## Non-Goals

These are deliberately not requested here:

- A general C preprocessor in Aether.
- Automatic parsing of all Redis headers.
- Defining Aether varargs functions. Existing C-side `va_list` bridges are
  enough.
- Replacing Redis' build system.
- Changing Aether's `float` spelling immediately; it already maps to C
  `double`. The remaining precision gap is `long double` and explicit ABI
  naming.
