# Ticket: Aether-core ergonomics gaps surfaced by the mquickjs C→Aether port

Status: open
Filed: 2026-05-22
Source: the mquickjs C→Aether migration (mquickjs/ in this tree)

Migrating mquickjs's engine to Aether keeps hitting the same
boundary-ergonomics walls. Each forces a hand-written C shim to
survive in mquickjs.c, blocking the "pristine Aether-calling-Aether"
end state. None are blockers (workarounds exist), but each leaves a
residue of C that the migration cannot otherwise eliminate.

## 1. Take the address of an Aether function — ALREADY SUPPORTED

RESOLVED (2026-05-22): no compiler change needed. The existing
`funcname as fn(T1, T2, ...) -> R` cast, when applied to a bare
*function name* (not a ptr value), evaluates the function name — which
decays to its address in C — and lowers to `((void*)(funcname))`. The
result is a real C-ABI function pointer storable in a `ptr` and
passable to `qsort`, callback tables, etc. Verified end-to-end against
libc `qsort` with an Aether comparator
(`tests/regression/test_fn_address_via_as_fn.ae`).

This means the `*_addr()` C shims in the mquickjs port are unnecessary:
- `mquickjs.c` `mqjs_js_array_sort_cmp_addr` / `_swap_addr`,
  `mqjs_range_sort_cmp_addr` / `_swap_addr`
- `mquickjs_build.c` `atom_cmp` + `mqjs_build_atom_cmp_addr`
can all be deleted in favour of `<fn> as fn(...) -> R` at the call site.
(Original ticket text wrongly believed this was missing.)

## 2. `va_list` consumers cannot be authored in Aether — IMPLEMENTED

Status (2026-05-22): DONE (issue #536). A function declared with a
trailing `...` is now C-variadic; its body reads varargs via
`va_start()` / `va_arg(vap, T)` / `va_end(vap)` intrinsics, lowering to
the standard C `va_list` machinery. Covered by
`tests/regression/test_va_list_consumer.ae`. This lets the mquickjs
port move `js_vprintf` and retire its `mqjs_va_arg_*` C helpers.
(Original text below kept for context.)

Aether can *call* C variadics (the `extern foo(..., ...)` feature is
used heavily and works well), but cannot *define* a function that
consumes a `va_list`. Every printf-family function therefore stays in
C, and any Aether code that wants formatted output must route through
a fixed-arity C shim per format string.

Concrete cases:
- `js_printf` / `js_vprintf` / `js_vsnprintf` / `term_printf` — remain
  in C.
- This session folded ~30 `mqjs_js_printf_*` / `mqjs_dbc_*` /
  `mqjs_dump_mem_*` fixed-string shims into two surviving generic
  shims (`mqjs_js_printf_str`, `mqjs_js_printf_str_int`) — but those
  two must stay C because they hold the `va_list` boundary.

Ask: either Aether-side `va_list` consumption, or a blessed
`format(fmt, args...)` builtin so the printf boundary need not be C.

## 3. No portable `sizeof`/`INTPTR_MAX` / compile-time platform width

Aether code that must zero or stack-allocate a C struct needs the
struct's byte size, and there is no portable Aether-side `sizeof`.
Hardcoding (e.g. `mem.set(s, 0, 408)` for `sizeof(JSParseState)`)
is brittle and DID break this session (wrong guessed size → segfault),
so the C `memset(s, 0, sizeof(T))` shim had to stay.

Likewise `mquickjs_build.c`'s `build_atoms` trampoline survives only
to resolve `host_default_jsw` via the C preprocessor
(`#if INTPTR_MAX >= INT64_MAX`), which Aether cannot read.

Ask: a compile-time `sizeof(extern struct T)` usable from Aether, and
a portable pointer-width / platform constant.

## Notes / non-asks

- The `as fn(...) -> R` cast for *calling through* a C function
  pointer works well and eliminated several shims this session
  (interrupt-handler dispatch, JSWriteFunc dispatch). Item 1 above is
  the *inverse* (producing a pointer), which is still missing.
- libc-macro bridges (`stderr`, `stdout`) legitimately need a C
  symbol; those are not part of this ask.
