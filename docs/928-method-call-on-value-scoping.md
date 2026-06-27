# #928 — Method-call-on-value: scoping note

Two candidate mechanisms for `subject.matcher().matcher()`. This note
grounds each in the *current* compiler (parser dot-call handling,
typechecker call resolution, by-value struct codegen) so the pick is made
against real seams, not a sketch. All findings verified against the live
`build/aetherc` at the tip of `main`.

## The shared call site in the compiler

A dotted call `recv.member(args)` is parsed in the postfix loop of
`parse_postfix_expression` (`parser/parser.c` ~1576). Today the callee name
is only synthesised for two receiver shapes (parser.c:1602–1615):

- `expr` is a bare `AST_IDENTIFIER` → simple call `foo()`.
- `expr` is `AST_MEMBER_ACCESS` whose **child[0] is an `AST_IDENTIFIER`** →
  qualified name `"ns.func"` (this is how `string.length()` /
  `math.pow()` module calls are formed).

For any **other** receiver — notably a *call result*, `expect(5).to_*` —
`func_name` stays `NULL` and typecheck reports `Undefined function '?'`.
That is the first repro in the issue. So **whichever mechanism we pick, the
parser must be taught to carry the receiver subtree for the general dotted
call**, not just `identifier.member`.

Call resolution then happens in the typechecker (`analysis/typechecker.c`
~5160–5240). There is already a precedent for a *fallback* when
`recv.field(args)` doesn't resolve as a function symbol: #749 dispatches
through a function-pointer **struct field** (typechecker.c:5189–5218),
splitting `recv` from `field` on the last `.` and tagging the node
`fnfield_ptr`/`fnfield_val` for codegen. UFCS slots in as a **sibling
fallback right next to it**.

## The cost question (answered)

The issue asks for a compiler opinion on per-hop struct-copy cost. Verified
by emitting C for a 2-field `Subject` threaded through `bump(bump(a))`:

```c
typedef struct Subject { int val; int neg; } Subject;
Subject bump(Subject s) { return (Subject){.val = (s.val + 1), .neg = s.neg}; }
...
Subject b = bump(bump(a));
```

By-value structs lower to **plain C by-value** — no heap alloc, no
refcount, no hidden pointer threading. A chain of N matcher calls on a
small subject record is N register-width struct copies, which the C backend
elides under `-O2` (a 2–3 field record passes in registers). **The
hot-test-loop concern is unfounded; this is cheap.** Both mechanisms below
lower to exactly this shape — the codegen is *already done*, we only change
how the call is spelled and resolved.

---

## Option 1 — UFCS (`x.f(args)` ⇒ `f(x, args)`)

**Rule.** When `recv.method(args)` does not resolve as (a) a
module-qualified call, (b) a struct field / fnptr-field, then if there is a
free function `method` whose **first parameter type unifies with
`typeof(recv)`**, rewrite to `method(recv, args)` and typecheck that.

**Changes.**
- *Parser*: in the postfix `(` handler, when the receiver is not one of the
  two existing shapes, emit a call node that **retains the receiver
  subtree** as an implicit first argument, tagged (e.g. `ufcs_pending`) so
  the typechecker knows the dotted head is a *method name*, not a qualified
  module path. (Module-qualified `ns.func()` keeps its existing fast path —
  no behaviour change for `string.length()`.)
- *Typechecker*: a new fallback branch beside the #749 fnptr-field block:
  resolve the bare `method` name as a free function, check
  `param[0] == typeof(recv)`, splice `recv` as arg 0, then run the normal
  arity/type checks on the combined arg list. Stamp the return type for
  chaining.
- *Codegen*: **none.** The rewritten node is an ordinary call —
  `to_equal(expect(5), 5)` — which already lowers correctly.

**Disambiguation (the one real subtlety).** `a.b(c)` is now three-way
ambiguous: module call, struct/fnptr field, or UFCS. Order is what keeps it
safe: keep the existing module + field resolution **first**, UFCS strictly
**last** (only on what currently errors). That makes UFCS purely additive —
nothing that compiles today changes meaning. A `method` that collides with
a real field name resolves to the field (status quo wins).

**Pros.** Smallest change; no new declaration syntax; every existing free
function becomes chainable; dissolves the `fw`-threading awkwardness the
issue calls out (the subject record carries context). Matches the author's
stated lean. D/Nim precedent.

**Cons.** Dispatch is on first-param type only — no overload set / no
receiver-type namespacing, so two modules each exporting `to_equal` would
need disambiguation by import. Reads as a method but is a free function
(some find that surprising).

**Rough size.** Parser: ~1 branch + retain-subtree. Typechecker: ~1
fallback branch mirroring #749. Codegen: 0. Tests: positive chain, the
no-match error, the module/field-still-wins precedence cases.

---

## Option 2 — Receiver methods (`fn (s: Subject) f(args)`)

**Rule.** A new declaration form binds `f` as a method of the receiver
type; `recv.f(args)` dispatches on `typeof(recv)`.

**Changes.**
- *Lexer/parser*: a **new declaration grammar** — `fn (recv: T) name(params)
  -> R { ... }`. This is the `E0100: Expected LEFT_BRACE, got IDENTIFIER`
  repro in the issue; the `fn` keyword path has to learn the
  parenthesised-receiver prefix.
- *Symbol table*: a **method table keyed by (receiver type, name)** —
  genuinely new machinery (today functions live in one flat namespace).
- *Typechecker*: dispatch `recv.f` through the method table; receiver
  becomes implicit arg 0.
- *Codegen*: lower to a mangled free function `T__f(T recv, ...)` — same
  by-value shape as Option 1, just a name-mangling step.

**Pros.** Familiar Go shape; receiver-type namespacing means two types can
both have `to_equal` with no collision; clearer intent at the declaration.

**Cons.** Biggest surface: new declaration grammar **and** a method table
**and** dispatch rules **and** name mangling. More places to get the
interaction with existing module/field resolution wrong. Doesn't make
*existing* free functions chainable — everything must be re-declared as a
method.

**Rough size.** Parser: new grammar production (non-trivial). Symbol
table: new keyed structure. Typechecker: new dispatch path. Codegen:
mangling. Tests: a broad matrix. ~3–4× Option 1.

---

## Recommendation

**Option 1 (UFCS), implemented as a last-resort fallback** beside the
existing #749 fnptr-field dispatch. It is the smallest change, it is purely
additive (nothing that compiles today shifts meaning, because UFCS only
fires on what currently errors), it makes the whole existing free-function
surface chainable, and it lands the motivating assertion-DSL family without
a new declaration grammar or a method table. The cost concern is already
disproven by the by-value codegen. Option 2's only real win — receiver-type
namespacing of method names — is not needed by the motivating use case and
can be layered later if overload collisions ever bite.

If we adopt UFCS, the build order is: (1) parser retains the receiver
subtree for general dotted calls; (2) typechecker UFCS fallback with
first-param unification and strict last-resort ordering; (3) tests for the
chain, the precedence (module/field win), and the no-match error. Codegen
is untouched.

---

## Status: IMPLEMENTED (this PR)

Option 1 (UFCS) is implemented exactly as scoped above — no codegen change.

- **Parser** (`parser/parser.c`, postfix call handler). A dotted call whose
  receiver is **not** a bare identifier (a call result, an indexed value —
  `expect(5).to_eq(...)`) previously produced no callee name and failed as
  `Undefined function '?'`. It now detaches the receiver subtree, carries the
  bare method name, tags the call node `ufcs`, and seats the receiver as
  `children[0]`. The `identifier.member()` path is unchanged (still forms the
  qualified name first).
- **Typechecker** (`analysis/typechecker.c`, `try_ufcs_rewrite`). Two entry
  shapes both rewrite to canonical `method(recv, args)`:
  - shape (a): the parser-tagged node (receiver already at `children[0]`);
    handled at the top of `typecheck_function_call`.
  - shape (b): a `recv.method(args)` where `recv` is a single-identifier
    **value** — tried as a strict last resort, right after the #749
    fnptr-field fallback and before the Undefined-function error.
  In both shapes UFCS only fires when `method` resolves to a free function
  whose first parameter type unifies with `typeof(recv)`; otherwise it
  declines and the standard error stands. Module-qualified resolution,
  struct-field, and fnptr-field dispatch all keep priority, so nothing that
  compiled before changes meaning.
- **Codegen**: untouched. The rewritten node is an ordinary call.

Verified: chained call-result receivers (`expect(5).inc().to_equal(6)`),
stored-value receivers, pointer receivers (`c.bump()` →
`bump(c)` with `c: *Counter`), module calls still resolving, and both
decline paths (no such free function; first-param type mismatch) producing a
clean `Undefined function` error rather than a miscompile. Regression test:
`tests/integration/ufcs_method_call/`.
