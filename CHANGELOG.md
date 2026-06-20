# Changelog

All notable changes to Aether are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

**Workflow**: New changes go under `## [0.264.0]` (create it at the top if
absent). When a release PR merges to `main`, the release pipeline
(`.github/workflows/release.yml`) automatically replaces `## [0.264.0]`
with the next version number before tagging. Always use the literal
`[current]` placeholder — a hardcoded version header here is *not*
renamed, so it drifts from the tags and can cause the next release's
notes to be skipped or clobbered (the failure modes documented in
`changelog-release-drift-note.md`).

## [0.287.0]

### Added

- **`std.bignum` — arbitrary-precision integers (foundation layer)** (issue
  #739 slice 11, the BigInteger watershed). Pure Aether, ported from Bouncy
  Castle's `BigInteger.cs`: sign-magnitude representation (32-bit limbs over
  `std.intarr`, big-endian, separate sign in {-1,0,1}). This first layer
  covers `from_bytes` / `to_bytes` (two's-complement **signed**) +
  `from_bytes_unsigned` / `to_bytes_unsigned` (magnitude) over `std.bytes`
  (consistent with the rest of the cryptography port), `from_int`, `compare`,
  `is_zero`, `sign`, `bit_length`, `add`, `subtract`, `negate`, `abs`,
  `shift_left`, `shift_right`. Every operation was cross-checked against
  Python's arbitrary-precision `int` (add/sub/compare/shift over signed
  integers including INT_MIN; signed+unsigned byte round-trips including the
  `80` / `0080` / `00ff` / -128 two's-complement edges). Regression:
  `tests/regression/test_bignum_foundation.ae`. No externs to OpenSSL or any C
  bignum library. **Multiply, divide/mod, mod_pow, gcd, mod_inverse, and
  is_probable_prime are deferred to follow-up layers** — this foundation is the
  Tier-2 gate that, once complete, unblocks RSA/DSA/ECDSA/X.509.

## [0.286.0]

### Added

- **Native HMAC + HMAC-DRBG** (`std.cryptography.hmac`,
  `std.cryptography.drbg`, issue #739) — pure Aether on the native SHA-2.
  - **`std.cryptography.hmac`** — RFC 2104 HMAC over any native SHA-2 digest,
    working entirely in `bytes` buffers (so it's binary-safe for arbitrary
    keys/messages, including the key-longer-than-block hashed-key path).
    One-shot (`hmac_sha256` / `_hex`, `hmac_sha512`, generic `hmac_bytes` /
    `hmac_hex`) and streaming (`new(algo, key, key_len)` → `update` →
    `final_bytes` / `final_hex`). Verified against `openssl dgst -mac HMAC`
    on RFC 4231 vectors.
  - **`std.cryptography.drbg`** — SP800-90A HMAC-DRBG, ported from Bouncy
    Castle's `HMacSP800Drbg.cs`. Deterministic (caller supplies entropy):
    `new(algo, entropy, …, nonce, …, perso, …)` → `generate` /
    `generate_with_input` / `reseed`. Verified against Bouncy Castle's own
    `HMacDrbgTest.cs` SHA-256 vector (two generates match byte-for-byte).
  - Also adds `sha2.update_bytes(ctx, bytes, len)` — a binary-safe streaming
    update the HMAC construction needs.

  No externs to OpenSSL or any C crypto library. Regression:
  `tests/regression/test_hmac_drbg_native.ae`. Bouncy Castle (MIT) attribution
  on the DRBG; HMAC is the generic RFC 2104 construction. CTR-DRBG is deferred
  until native AES lands.

## [0.285.0]

### Added

- **Native SHA-2 family** (`std.cryptography.sha2`, issue #739 slice 2) —
  SHA-224, SHA-256, SHA-384, SHA-512, SHA-512/224, SHA-512/256, implemented in
  **pure Aether** on the Tier-0 foundations (`std.bits` for logical
  shifts/rotates, `std.longarr` for the 64-bit message schedule, `std.bytes`
  big-endian accessors). No externs to OpenSSL or any C crypto library — the
  compression functions, padding, and length encoding are all Aether code.
  Ported from Bouncy Castle's `GeneralDigest` / `LongDigest` /
  `Sha{224,256,384,512}Digest`. Both one-shot (`sha256_hex` / `sha256_bytes`,
  …) and streaming (`new(algo)` → `update` → `final_hex` / `final_bytes`,
  ctx self-freed on finalize). Every digest was cross-checked against
  `openssl dgst` across all block-boundary input lengths (55/56/63/64/65/
  119/120/127/128). Regression: `tests/regression/test_sha2_native.ae` (NIST/
  RFC vectors + streaming-equals-one-shot). Bouncy Castle (MIT) attribution
  per file. This unblocks native streaming-digest + DRBG (Tier 1 items 5/6),
  which the existing OpenSSL-backed digest ctx will be retired in favour of.

## [0.284.0]

### Added

- **Cryptography port Tier 0 foundations** (issue #739, slice 1) — four
  Aether-native stdlib modules every digest/cipher port depends on:
  - **`std.longarr`** — fixed-size 64-bit packed array, the `long`-cell twin
    of `std.intarr` (SHA-512 / Keccak / GCM / Poly1305 / lattice-PQC state).
  - **`std.bits`** — unsigned-bit helpers Aether's signed `int`/`long` can't
    express directly: `lsr32/64` (logical right shift — Aether's `>>` is
    arithmetic), `rotr/rotl 32/64`, `popcount32/64`, `clz32/64`, `udiv/urem
    32/64`, `ucmp64`. Ported from Bouncy Castle's `Integers.cs` / `Longs.cs`.
  - **`std.bytes` big-endian accessors** — `set_be16/32/64` + `get_be16/32/64`,
    the BE twin of the existing `_le*` family (cryptography wire format is mostly
    big-endian). Modelled on Bouncy Castle's `Pack.cs`.
  - **`std.bytes.cursor`** — forward read-position over a bytes buffer
    (`read_u8`, `read_be_u16/32/64`, `read_slice`, `remaining`, `peek`, `eof`,
    `pos`/`seek`); foundation for byte parsers (ASN.1, PEM, OpenPGP).

  Regression tests (`tests/regression/test_{bits,longarr,bytes_be,bytes_cursor}.ae`)
  include vectors ported from Bouncy Castle's `IntegersTest`/`LongsTest`.
  Bouncy Castle (MIT) attribution per ported file plus a new top-level
  `THIRD_PARTY_LICENSES.md`.

## [0.283.0]

### Fixed

- **Module token cap raised 20000 → 100000, buffer heap-allocated**
  (`compiler/aether_module.c`). `module_parse_file` capped imported modules at
  `MAX_MODULE_TOKENS` tokens; a larger module was silently truncated
  mid-token-stream, dropping its tail declarations so callers hit spurious
  `E0301: Undefined function` on the missing symbols. Raised the cap 5× and
  moved the token buffer off the stack to a `malloc`'d array (a fixed
  100k-entry stack array would risk overflow), with NULL-check cleanup and a
  matching free. Regression: `tests/integration/module_token_cap` imports a
  ~2200-function module (>20k tokens) and calls its first, middle, and last
  function — truncation under the old cap left the tail undefined.

## [0.282.0]

### Fixed

- **`fn name(...)` is now a first-class top-level function definition**
  (#791). `fn` is not a reserved word (it doubles as the function-pointer
  type head `fn(...) -> R`), so a top-level `fn name()` previously only
  survived via parse-error recovery: the parser raised "unexpected
  identifier at top level" on `fn`, recovery skipped the token, and
  `name(...)` then parsed as a function. That recovery is silent when a
  module is imported but fatal on a standalone / strict re-parse, so at
  full module-graph scale a re-parsed sibling module that used the `fn`
  spelling (e.g. std.uuid, std.url) surfaced the recovery as a spurious
  top-level parse error attributed to that module. The top-level parser
  now recognises `fn` + name + `(` as a function definition directly, so
  the spelling is first-class and parses identically on every path
  (standalone build, import, and re-parse). `fn`-typed parameters
  (`f: fn(int) -> int`) still parse as types — the definition form is
  distinguished by the identifier between `fn` and `(`.

## [0.281.0]

### Fixed

- **`std.fs`: export `join_clean` and `first_element`.** The two
  path-cleaning wrappers added in `std.fs: add join_clean + first_element`
  were defined but omitted from the module `exports (...)` list, so
  `fs.join_clean(...)` / `fs.first_element(...)` failed at the call site
  with `E0301: Undefined function` (the `tests/integration/fs_join_clean`
  regression went red). Added both to the export list.

## [0.280.0]

### Changed

- **VS Code extension: grammar, snippets, and ergonomics refresh**
  (`editor/vscode/`). Refreshed the TextMate grammar for missing keywords,
  types, and `@annotations`; added range/spread operators, the `make` keyword,
  and snippets; block-comment on-enter + indent rules; a cross-platform
  "Erlang-style" palette; and README palette/install-snippet docs. Also
  rebuilt `out/extension.js` with an activation-leak fix. Editor tooling only —
  no compiler, std, or runtime change.

## [0.279.0]

### Added

- **`heap.new(T)` / `heap.free(p)` — POD struct heap allocation** (issue
  #564; `compiler/parser`, `compiler/analysis`, `compiler/codegen`).
  `ctx = heap.new(AppCtx)` allocates a zero-initialised `AppCtx` on the
  heap and returns `*AppCtx`; fields are read/written through the pointer
  (`ctx.port = 8080`) and the box is reclaimed with `heap.free(ctx)`
  (NULL-safe). Lowers to `((T*)calloc(1, sizeof(T)))` / `free(p)` — the
  guaranteed zero-init the memory-safety review requires. **POD-only**: the
  type must be a struct with no `string` (or other heap-managed) field; a
  non-POD struct is a compile error directing the author to hold heavy data
  as an opaque handle the struct doesn't own. This is the safe first cut
  from the issue's recommendation #1 — richer boxes that own their fields
  need an ownership model (retain-on-store + typed free) specced first.
  Replaces the `malloc(64) as *AppCtx` magic-number pattern with a
  type-safe, self-documenting primitive. Pairs with the
  `defer heap.free(p)` idiom for scope-bounded lifetimes.

## [0.278.0]

### Added

- **`expr!` unwrap-or-trap operator** (`compiler/parser`, `compiler/analysis`,
  `compiler/codegen`). A postfix `!` on a `(value, err)` tuple yields the
  first slot and panics if the trailing (string) error slot is non-empty:
  `h = cryptography.random_hex(n)!` replaces the two-line
  `h, e = ...  return h` discard wrapper. Works on any tuple whose final
  slot is the `string` error (2-tuples, the `(bytes, len, err)` 3-tuple,
  …); the result type is the first slot. Composes anywhere an expression
  is allowed — assignment RHS, call arguments — via a GCC
  statement-expression that evaluates the tuple once. `!` stays the actor
  fire-and-forget operator when followed by a message type (an
  uppercase-leading identifier); the unwrap reading applies everywhere
  else. A non-tuple or string-less-final-slot operand is a compile error.

### Fixed

- **`import std.fs (*)` (glob import) now carries the real tuple return
  types of `(value, err)` wrappers** (`compiler/analysis/typechecker.c`).
  A glob import registered each short alias by cloning the full symbol's
  type *before* return-type inference ran, so a wrapper whose return type
  is inferred (e.g. `fs.list_dir`'s `(ptr, string)` tuple) left the bare
  alias `list_dir` stuck on a pre-inference `int` placeholder. A
  `list, err = list_dir(...)` then stamped the call's return type as
  `int` and codegen emitted `int _tup0 = fs_list_dir(...)` — a C type
  error. Namespaced (`fs.list_dir`) and selective imports already worked;
  the glob form did not. Import-alias short symbols are now re-synced from
  their inferred full symbols after type inference, so all three import
  forms agree. (fbs-core ask #1.)

### Added

- **Streaming (incremental) digest context in `std.cryptography`**
  (`std/cryptography/`). `digest_new(algo)` returns an opaque context;
  `digest_update(ctx, data, n)` feeds bytes in pieces; `digest_final_hex(ctx)`
  / `digest_final_bytes(ctx)` finalize (and free the context). `algo` uses
  the same names as `hash_hex` ("md5", "sha256", "sha1", "md4", ...).
  This hashes data that arrives in windows without ever holding it whole —
  a blob store can now compute an upload's ETag as it streams to disk
  instead of reading the stored object back purely to MD5 it (S3 ETag =
  md5-of-object; multipart ETag = md5-of-md5s). `digest_free(ctx)` is the
  abandon-without-finalize cleanup path. Thin veneer over libcrypto's
  `EVP_DigestInit/Update/Final`; returns the "openssl unavailable" error
  shape on builds without OpenSSL.
- **`fs.join_clean(a, b)` and `fs.first_element(path)`** (`std/fs/module.ae`).
  `join_clean` is `path_join` followed by `clean` in one call — the
  cleaned path that actually reaches the filesystem after a caller-
  supplied segment is appended, so `fs.join_clean("bucket", "a/../b")`
  collapses to `bucket/b` rather than leaving the traversal in place
  (path-traversal-defense invariant for object stores). Empty-segment
  handling mirrors `path_join`'s identity behaviour. `first_element`
  returns the leading cleaned path component (`fs.first_element("/a/b")`
  → `"a"`). Together they let downstream blob-store code drop its
  hand-rolled `pathutil.join` wrapper.

## [0.277.0]

### Fixed

- **contrib host bridges: lua + tcl compile against newer Homebrew /
  Tcl-9 libraries** (`contrib/host/lua/aether_host_lua.c`,
  `contrib/host/tcl/aether_host_tcl.c`). Both dlopen bridges name C-API
  functions as struct fields and call them as `g_lib.Fn(...)`, which the
  preprocessor mangles when the library header turned `Fn` into a
  function-like macro:
  - **Lua 5.4** (Homebrew): `luaL_openlibs(L)` is
    `#define`d to `luaL_openselectedlibs(L, ~0, 0)`, so
    `g_lua.luaL_openlibs(L)` rewrote to a non-existent
    `luaL_openselectedlibs` member (macOS build break; Debian's Lua 5.4
    ships it as a real declaration, which is why Linux CI never saw it).
    Fix: `#undef luaL_openlibs` after the headers and dlsym the real
    exported symbol (present in every shipping liblua).
  - **Tcl 9.0** (Homebrew): `Tcl_GetStringResult` and `Tcl_GetString`
    became function-like macros over `Tcl_GetStringFromObj` and are no
    longer exported, so the struct-field calls referenced non-existent
    members. Fix: `#undef` both, resolve the lowest-common-denominator
    real exports `Tcl_GetStringFromObj` + `Tcl_GetObjResult` (present in
    both 8.6 and 9.0), and recompose the two accessors as local helpers.
  Both verified by compiling each bridge against the real (8.6 / 5.3 /
  5.4) headers and against simulated Homebrew-macro headers — clean with
  the fix, reproduces the reported break without it. No behavioural
  change on platforms that were already building (the `#undef`s are
  no-ops where the macro is absent). Surfaced on a macOS/Homebrew
  `make install-contrib` (Lua 5.4.x, Tcl 9.0.3).

## [0.275.0]

### Added

- **`@packed` extern structs** (#747 item 1, the Redis sds.c blocker). An
  `extern struct ... @packed { ... }` emits the C body with
  `__attribute__((packed))`, so the layout has no inter-field padding or
  trailing alignment — the `sdshdr8/16/32/64` shape where the length/
  alloc/flags header sits at fixed packed offsets before the string data.
  `sizeof(S)` / `offsetof(S, f)` lower to C and report the packed numbers,
  and a `*S` overlay reads/writes fields at their packed offsets (verified
  by round-trip). `@packed` is mutually exclusive with `@c_import` (a
  header-defined struct's packing is the header's job; combining them is a
  parse error). Bit-width fields and a trailing flexible array still work
  under `@packed`. Note: pure `@c_import` overlays already inherit the
  header's packed layout (no body emitted), so `@packed` is the tool when
  Aether owns the struct body (a pure-Aether port with no C header). See
  [docs/c-interop.md](docs/c-interop.md) (Packed structs).

## [0.276.0]

### Added

- **Function-pointer struct fields** (#749 Ask A). A struct field typed
  `fn(T1, T2) -> R` now emits the C function-pointer member
  `R (*name)(T1, T2)` (instead of an untyped `void*`), and a call through
  it is a real indirect call: `o.field(args)` for a value struct,
  `p.field(args)` → `p->field(args)` for a pointer-to-struct. This is the
  `dictType` vtable shape that gates the Redis dict.c port (2340 lines)
  and the keyspace command tier. Field PARSING already worked (parse_type
  yields the fn-ptr type); the change is codegen + dispatch: a typed
  field-declarator emitter, plus — because the parser collapses a
  member-access callee to the dotted name `recv.field` and drops the
  receiver — a typecheck branch that recognises `recv.fnptrfield(args)`
  (resolving the field signature off the struct definition, tagging the
  receiver as value vs pointer) and a matching codegen branch that emits
  the indirect call. Single-level receiver (a bare local); the field
  already carries a real C fn-ptr type, so the call needs no cast. Sibling
  of fn-pointer parameters (#750) and typed fn-ptr locals. See
  [docs/language-reference.md](docs/language-reference.md) (Function-
  pointer struct fields).
## [0.274.0]

### Added

- **`longdouble` primitive type** (#749 Ask C, completing the aedis
  core-floor umbrella). Maps to C `long double` — the widest floating
  type — for the exact-decimal numeric paths a C interop layer needs
  (libc `strtold`, INCRBYFLOAT / sorted-set score conversion,
  object.c/util.c number formatting). Supports arithmetic (`+ - * /`),
  comparison, and conversion to/from `int` and `float`; as the widest
  numeric it wins promotion (`longdouble op int` / `op float` →
  `longdouble`). Usable in locals, params/returns, struct fields, and
  `extern` signatures; formatted with `%Lg`/`%Lf` in interpolation and
  `print`. No source literal — values arrive via an extern or by widening
  an `int`/`float`. Spelled as the type name `longdouble` (no new keyword
  token). See [docs/language-reference.md](docs/language-reference.md)
  (`longdouble`).

  With this, the #749 umbrella is fully addressed: fn-pointer parameters
  (#773) and struct fields (#777) for Ask A, the inline `...` C-varargs
  call-through already shipped for Ask B, and `longdouble` for Ask C.

## [0.273.0]

### Added

- **By-value struct returns and stack-struct locals** (#746). A function
  may now declare a by-value struct return type (`make() -> Pair`), and a
  struct can be declared as a stack-allocated local (`Pair p` — no `*`, no
  initializer) and filled field-by-field. Both were parse errors before:
  the `-> StructName` return type fell through the `->` return-type
  disambiguator (an off-by-one in its `{` lookahead — `->` is already
  consumed, so the name sits at offset 0, not 1) into the `-> expr`
  arrow-body path; and `StructName name` had no statement-level
  declaration case (only `*StructName name` and the C-ABI aliases like
  `size_t n`). Both fixes are parser-only — the `IDENT IDENT` stack-local
  case mirrors the existing `*StructName name` pointer path, and codegen
  was already correct (struct return type via get_c_type, `.field` access
  on a value struct, `return p` as a C struct copy). Completes the
  by-value struct set (by-value params already worked), so an all-scalar
  record (a geometry/bounding-box result, the geohash_helper.c shape) can
  be built on the stack and returned without heap allocation or an
  out-pointer. See [docs/language-reference.md](docs/language-reference.md)
  (By-value struct returns and stack-struct locals).
## [0.272.0]

### Added

- **Function-pointer parameters** (#750). A `fn(T1, T2) -> R` parameter now
  lowers to the exact C function-pointer type `R (*name)(T1, T2)` in both
  the prototype and the definition, and a call through it (`cb(a, b)`) is a
  real typed indirect call. Previously a fn-typed parameter collapsed to a
  bare `void*` and the body call emitted invalid C ("called object is not a
  function"); the `as fn(...)` cast only rescued a single in-body callback,
  which didn't scale to multiple callback params or callback-taking helpers.
  This is the parameter form of the existing typed-fn-pointer machinery
  (`as fn(...)` locals, fn-pointer struct fields): the parser/typechecker
  already carried `is_fnptr` onto the parameter, so the fix is codegen-only —
  a fn-ptr declarator emitter for the prototype + definition, plus
  registering the param in the fn-ptr-local registry so the call site emits
  the typed indirect call. Unblocks porting callback APIs (Redis dictScan/
  raxWalk/command-table iteration; qsort, signal handlers, libcurl/sqlite
  hooks). See [docs/language-reference.md](docs/language-reference.md)
  (Function-pointer parameters).

## [0.271.0]

### Fixed

- **Parser: terminate expression continuations at newlines**
  (`compiler/parser/parser.c`). A line-leading token was sometimes folded into
  the previous line's expression as a continuation, so statements that begin a
  fresh line (e.g. a following `[...]` or call) could be mis-grouped. The
  parser now ends an expression continuation at a newline, matching the
  line-oriented statement model; net simplification of the continuation logic.
  Covered by `tests/syntax/test_parser_line_leading_statements.ae` and a new
  `test_parser_newline_bracket` regression.

## [0.270.0]

### Fixed

- **Parser: newline now terminates infix/postfix expression continuation**
  (issue #528; `compiler/parser/parser.c`,
  `tests/regression/test_parser_line_leading_statements.ae`,
  `tests/integration/parser_newline_bracket/`). The old guarded
  recogniser handled `*StructName name`, `*ident = ...`, and a narrow
  `[x, y]` shape, but still let line-leading unary statements like `-x`
  fold into the previous expression. The binary-expression loop now
  stops whenever an infix operator starts on a later source line, and
  postfix indexing does the same for newline-led `[`. Multiline
  continuations remain supported by placing the operator before the
  newline (`total = a +` newline `b`).

### Changed

- **Codegen cleanup: removed the now-dead #759 tuple-struct heap-flag
  transfer, superseded by #762's return-escape contract**
  (`compiler/codegen/codegen_stmt.c`). Two independent fixes for #752
  (struct-with-heap-string-field returned via tuple) both landed: #759
  zeroed the source struct's `_heap_<field>` flags before the
  function-exit `<Struct>_destroy` defer, and #762 (later, more complete)
  suppresses that destroy entirely on the escaping struct and pushes the
  destroy to the *receiving* caller. With #762's suppression the destroy
  never runs in the callee, so #759's flag-zeroing became a dead store
  (`r._heap_s = 0;` on a struct whose destructor is gone). Removed the
  `emit_tuple_struct_heap_ownership_transfer` helper and its sole call
  site; #762's `mark_returned_struct_escaped` on the same tuple-return
  loop is the live, complete mechanism. No behavioural change — verified
  the generated C drops the dead store while the caller-side single free
  is unchanged; both #752 regression tests
  (`tests/integration/issue_752_struct_string_tuple/`,
  `tests/regression/test_struct_string_field_return.ae`) and unit 229/229
  stay green. Pure dead-code removal; keeps the two-mechanisms-on-one-path
  hazard from misleading a future editor.

## [0.269.0]

### Added

- **RAM-bounded streaming request bodies (#626 upload half)**
  (`std/net/aether_http_server.c`, `std/net/aether_http_server.h`,
  `std/http/module.ae`, `tests/integration/http_stream_upload/`). The
  HTTP/1.1 server no longer buffers a large request body whole before
  dispatching the handler. When a request's `Content-Length` exceeds one
  connection buffer (16 KiB), the dispatcher parses only the header
  block and hands the handler a *streaming* request; `http.request_body_read(req,
  off, max)` then pulls each window straight off the socket. Peak server
  memory for a large upload is one window per connection (O(buf + chunk))
  instead of O(Content-Length) — for N concurrent M-byte PUTs that's the
  difference between N×M and N×window bytes live. The canonical loop the
  fbs-core ask sketched works unchanged:
  ```aether
  total = http.request_body_length(req)
  off = 0
  while off < total {
      chunk, n, _ = http.request_body_read(req, off, 65536)
      if n == 0 { break }
      fs.pwrite(out, chunk, n, off)   // stream → disk, never whole in RAM
      off = off + n
  }
  ```
  Small bodies keep the legacy fully-buffered path (random-access offsets,
  no behavioural change); streaming reads must be sequential (the socket
  isn't seekable). A post-handler drain consumes any body bytes the
  handler left unread so the keep-alive connection boundary stays clean
  for the next request (verified: a follow-up GET on the same socket
  after a 3 MiB streamed PUT still answers correctly). New
  `HttpRequest` streaming fields are trailing/ABI-stable (same promise as
  the connection-metadata block). Closes the upload half of #626
  (download/sendfile half shipped earlier); sourced from
  `stdlib-streaming-upload-body-followup.md` (fbs-core, which measured the
  buffered-upload peak the streaming path removes). Integration test PUTs
  3 MiB and asserts bounded streaming + byte-identical SHA-256 round-trip
  + clean keep-alive boundary.

## [0.268.0]

### Added

- **Typed module-level constant arrays** (#745). `const NAME: T[N] = [...]`
  declares a file-scope `static const <T> NAME[]` lookup table with the C
  element width pinned — `T` ∈ {`uint8`, `uint16`, `uint32`, `uint64`,
  `int`, `long`}. Previously the only spelling was `const NAME[] = [...]`,
  which always inferred `int` elements: a uint8/uint16 table cost 4× the
  memory and mismatched a C header expecting a packed `uint16_t[]` (e.g.
  the cluster-slot CRC16 table). The table is allocated once and shared
  across calls (not re-initialised per call). Two compiler changes: the
  top-level `const` parser now accepts a `: T[N]` annotation (and a typed
  scalar `const NAME: T = value`), and the short unsigned width names
  `uint8`/`uint16`/`uint32` are recognised type spellings (siblings of the
  existing `uint64` keyword, emitting `uint8_t`/`uint16_t`/`uint32_t`); an
  integer-element array literal may initialise a narrower integer-element
  typed const array (the explicit, compile-time-constant intent). See
  [docs/language-reference.md](docs/language-reference.md) (Module-level
  constant arrays).

## [0.267.0]

### Fixed

- **Heap string fields of a struct returned from a function are no longer
  corrupted** (#752, follow-up to #634). When a function returned a struct
  with a heap-string field (directly via a single-value builder return, or
  as a tuple element `return r, ""`), the struct's `<Struct>_destroy`
  function-exit defer freed the field even though the struct escaped via
  the return — so the caller read a dangling pointer and the string came
  back as garbage. Int fields survived (no free); a literal-initialised
  string survived (static), which is why the #634 test (int-only) missed
  it. Two-sided fix matching the established return-escape contract for
  plain heap strings: (1) the callee suppresses the struct's destroy when
  it escapes via a return (`return_escaped_struct_vars` → consulted by
  `try_emit_struct_destroy`), transferring ownership to the caller; (2) the
  caller that *receives* an owned struct — a tuple-unpack target or a local
  initialised from a struct-returning call — gets a `<Struct>_destroy`
  defer so the fields are freed exactly once at its scope exit. Verified
  leak-free and double-free-free (ASan + `leaks`) across tuple, single, and
  chained receive-then-re-return forms. Regression test
  `tests/regression/test_struct_string_field_return.ae` asserts the string
  field's *value* (a behavioural gate, unlike the compile-only #634 test).
## [0.266.0]

### Fixed

- **Module-global first-assigned inside a nested block no longer shadowed
  by an uninitialized local** (#744, regression in #701). Codegen's
  branch/loop variable hoisters (`hoist_if_branch_vars`,
  `hoist_if_else_common_vars`, `hoist_loop_vars`) pre-declared a `var`
  global as a fresh function local when its first assignment appeared
  inside an `if`/`while` body — shadowing the file-scope `static`, so
  every write landed in the local and the global kept its initializer
  forever. A silent miscompile whose visibility depended on optimization
  (it corrupted the aedis MT19937-64 PRNG port: a lazily-malloc'd state
  buffer's writes never reached the global). All three hoisters now skip
  names that are module globals — the write is already routed to the
  static by the variable-declaration emitter (`is_module_global_var`), so
  the local must not be emitted. Regression test in
  `tests/integration/module_globals/nested_block_init.ae` (if-body,
  loop-body, and a lazily-initialised counter; exits non-zero if a write
  fails to reach the global).
## [0.265.0]

### Fixed

- **#752: heap-string fields of a struct returned via tuple were freed
  before the caller could read them** (`compiler/codegen/codegen_stmt.c`,
  `tests/integration/issue_752_struct_string_tuple/`). A function
  returning `(R, err)` where `R` contains a `string` field initialised
  from a heap source (e.g. `string.copy(...)`) emitted:
  ```c
  _tuple_R_string _builder_ret = (_tuple_R_string){r, ""};
  /* deferred */ R_destroy(&r);   // ← frees r.s
  return _builder_ret;
  ```
  The tuple literal memcpys `r` into the returned tuple including its
  `.s` pointer; the immediately-following `R_destroy(&r)` defer then
  frees that pointer's buffer while the caller's copy still references
  it. Use-after-free; caller saw garbage in every string field while
  scalar fields survived. New helper
  `emit_tuple_struct_heap_ownership_transfer` walks every tuple-return
  child that is a bare `AST_IDENTIFIER` of struct type and emits
  `<varname>._heap_<field> = 0;` for each heap-string field after the
  tuple literal is built and before the defer drain. The struct's
  `_destroy` defer becomes a no-op for the transferred fields; the
  caller's returned struct retains `_heap_<field> = 1` so its own
  destruct path correctly reclaims the buffer. Sourced from fbs-core's
  attempt to convert `object_get` from a positional 8-tuple to
  `(Object, err)` (issue #752 repro). The fix only touches the
  with-defer multi-value-return path — the no-defer path constructs
  the tuple inline in `return (Tuple){...};` and has no destroy defer
  to race against.

### Added

- **`std.json.from_int(n)` — integer-flavoured number constructor**
  (`std/json/aether_json.c`, `std/json/aether_json.h`, `std/json/module.ae`,
  `tests/integration/json_from_int/`). Sibling of `json.num(value: float)`:
  takes a `long` (full int64 range) and stamps a `JV_FLAG_INTEGER` flag on
  the `JsonValue` so the serializer emits `%lld` instead of `%g`. The
  motivating bug: `json.num(53248000.0)` serialised as `"5.3248e+07"`
  (`%g` switches to scientific notation past ~1e7), wrong for byte-count
  / ID / total fields and lossy past 2^53. Adds a dedicated `integer`
  slot to the JsonValue union (shares the slot, no struct growth) and
  branches the encoder + `json_get_int` + `json_get_number` + clone-tree
  paths on the flag. Parser-side automatic flagging (recognising bare
  integers in input JSON) is a separate follow-up — the value still
  round-trips correctly via the float path as long as it fits in 2^53.
  Sourced from `stdlib-json-integer-value-ask.md` (fbs-core /metrics).

## [0.264.0]

### Documentation

- **Reconciled drifted CHANGELOG version sections** (0.222, 0.234, 0.235,
  0.257, 0.263). Backfilled clean missing sections and relocated entries that
  had landed under the wrong version header, restoring a gapless changelog
  through that range. Docs only — no code change.

## [0.263.0]

### Fixed

- **Codegen: fixed-array locals hoisted out of a loop/branch body are
  declared `T name[N]`, not the invalid `T[N] name`** (PR #753,
  `compiler/codegen/codegen_stmt.c`,
  `tests/regression/test_hoist_array_local_in_loop.ae`). A `byte[N]` /
  `T[N]` local declared inside a loop or branch — and not as the block's
  first statement — is pre-declared at function scope by the var-hoisters.
  They emitted `<get_c_type> <name>;`, but `get_c_type(TYPE_ARRAY)`
  returns `T[N]` (valid only in postfix-declarator position), so the
  hoist produced `unsigned char[8] buf;` and the variable came out
  undeclared at its use sites. New `emit_hoisted_local_decl()`
  special-cases `TYPE_ARRAY` to emit `elem name[N];`; both hoist sites
  route through it. The first-statement-in-block decl path was already
  correct. Found via the aedis Redis port's per-loop scratch buffers.

## [0.262.0]

### Changed

- **stdlib caps-audit — `std.net` HTTP client response & request buffers**
  (#461). Routed the two *unbounded* internal buffers in the HTTP client
  (`std/net/aether_http.c`) through the capability allocator: the response-
  body accumulation buffer (`full_response` — the attacker-controlled DoS
  surface: a malicious server can flood the response) and the request-header
  build buffer (`hdr`). Both are self-contained within `http_request_internal`
  with the live size tracked in a local (`cap` / `hdr_cap`), so the
  realloc-delta accounting and the error-/exit-path frees balance exactly;
  the empty-response fallback records its 1-byte size. The request body was
  already capped (prior PR). Bounded, caller-supplied request fields
  (method/url/header structs and strdups) and the redirect/dechunk/header-
  extract helpers are intentionally left on libc — they cross alloc/free
  boundaries into wrapper/test code where caps accounting can't stay
  balanced, and they are not the unbounded surface. Verified: unit 227/227
  (no accounting underflow), `-Werror` clean, http_client_dechunk +
  http_client_redirects integration tests pass (real round-trip through the
  capped response buffer).
## [0.261.0]

### Added

- **`std.cryptography.random_hex(n)` / `random_base64(n)` — printable-secret
  convenience wrappers over `random_bytes`** (`std/cryptography/module.ae`,
  `tests/integration/cryptography_random_hex/`). Two thin Aether-side wrappers
  that draw `n` cryptographically-secure bytes from the OS CSPRNG and return a
  lowercase-hex (2`n` chars) or RFC 4648 §4 unpadded-base64 string respectively.
  Motivating shape: opaque-bearer-token / API-key minting (e.g. SigV4 secret
  keys), where callers want a printable secret and the "obvious random function"
  should resolve to the secure path — not `std.math` (a clock-seeded PRNG fit
  only for sampling). Composes existing primitives; no new C, no new externs.
  Hex emission uses `std.bytes` (O(n) build vs the O(n²) repeated `string.concat`
  path). Sourced from `stdlib-csprng-secure-random-ask.md` (fbs-core), whose
  request items 1 + 2 (`random_bytes` + UUIDv4) already shipped at 0.213.0;
  this lands the convenience wrappers that were the third bullet of the same
  ask. Aether wrappers only (the existing `cryptography_random_bytes_raw` C
  side is unchanged).

- **`long long` type spelling on extern parameters / returns**
  (`compiler/parser/parser.c`, `tests/integration/long_long_extern/`). When
  the parser sees a second `long` after the first, both are consumed and the
  resulting type carries the verbatim C spelling `long long` instead of the
  default `int64_t`. The underlying TypeKind is still `TYPE_INT64`, so all
  arithmetic and typechecking behave identically — only the emitted C
  declaration text changes. Closes the "Minor, real, cheap" item from
  `aedis-core-floor-feature-asks.md`: a libc / POSIX header that spells a
  parameter as `long long` (e.g. `mstime_t` typedef chains, the MT19937 /
  SHA reference impls bundled with Redis) now matches its Aether-side
  prototype byte-for-byte, removing the gcc "conflicting types" error that
  previously forced the generated TU to compile *without* its header.
  Four-case integration test (single arg + return, large-value retention,
  mixed `long long` ↔ `int64_t` round-trip).

## [0.260.0]

### Changed

- **stdlib caps-audit — `std.os` POSIX allocation sites** (#462). Routed the
  unbounded, plugin-influenced heap allocations in `std/os/aether_os.c`
  through the capability allocator (`aether_caps_malloc/realloc/free`) so a
  sandboxed plugin can't inflate them past a memory cap: the command-output
  capture buffers (`os_exec_raw`, `os_run_capture_raw`,
  `os_run_capture_status_raw`, `os_run_pipe_drain_and_wait_raw`,
  `os_run_full_raw`'s stdout/stderr accumulator), the `os_getcwd_raw` path
  buffer, the `os_execv` argv scratch, and the `os_getenv` value. Caller-owned
  returns keep the documented libc-free / fail-safe-upward-drift contract
  (the `io_read_file_raw` / `io_getenv` model); internal/transient buffers
  free through the cap with their exact live size (realloc-failure paths free
  the *old* size). New `caps_os_getenv_denied_past_cap` unit test asserts an
  env read is refused when the cap is below the value size, with the counter
  unperturbed. Bounded sites (the 1-byte empty-heap sentinel, the
  pointer-only argv/envp arrays) and the Windows-specific helpers
  (`utf8_to_wide`/`wide_to_utf8`/`WBuf`/`win_launch`/drain-thread) are left as
  tracked follow-ups; `std/fs/aether_fs.c` remains. Verified: unit 228/228
  (ASan-clean), `leaks(1)` clean on the os example, full `.ae` regression 0
  failures.

---

Older releases (**0.259.0 and earlier**, down to 0.18.0) live in
[CHANGELOG-archive.md](CHANGELOG-archive.md).
