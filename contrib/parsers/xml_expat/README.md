# contrib.parsers.xml_expat — SAX-style XML parsing via libexpat

A thin Aether veneer over [libexpat](https://libexpat.github.io/),
the C XML pull-parser. Mirrors libexpat's native SAX model: caller
registers per-event handlers (start element / end element / character
data), then feeds bytes into the parser; libexpat invokes the
registered handlers as it walks the input.

The namespace exported by `import contrib.parsers.xml_expat` is `expat.*`
(last dotted segment of the module path) — `expat.parser_new()`,
`expat.on_start(...)`, etc.

## Two surfaces — choosing one

The module exposes two API tiers. They sit side-by-side; pick
whichever fits your use case:

| Need | Surface | Look |
|------|---------|------|
| Per-parse state lives as ordinary locals (a counter, a flag, a strbuilder), no struct juggling | **Closure builder** (`parse_with { bind_* }`) | `parse_with(input) { bind_start(|name, atts| { count = count + 1 }) }` |
| Long-lived parser, multi-chunk streaming, hand-rolled state struct, sharing handlers across parses | **Raw bare-fn** (`on_start` + `set_user_data`) | `expat.on_start(p, my_handler as fn(ptr, string, ptr) -> void)` |

The closure builder is a thin Aether wrapper around the raw surface
(it allocates a libexpat parser, installs a C-side dispatcher,
runs the parse, and tears everything down before returning). The
two surfaces are interoperable in the sense that they touch the
same libexpat parser type — you just can't mix them on the same
`XML_Parser*` in one parse.

See the two example sections below for side-by-side patterns.

## Build

`contrib/parsers/xml_expat` is **not** linked into `libaether.a`. Each
consuming project lists the wrapper in its own `aether.toml`:

```toml
[project]
name = "your_app"
version = "0.1.0"

[[bin]]
name = "your_app"
path = "src/main.ae"
extra_sources = ["contrib/parsers/xml_expat/aether_xml_expat.c"]

[build]
link_flags = "-lexpat"
```

Same rationale as `contrib.sqlite`: the wrapper depends on a system
library not every consumer wants forced on them. Install libexpat
from your distro (`libexpat-dev` on Debian/Ubuntu, `expat-devel` on
Fedora, `brew install expat` on macOS).

## Raw bare-fn surface

The 1:1 binding of libexpat's C API. Use this when the closure
builder doesn't fit — long-lived parsers, multi-chunk streaming,
shared handlers across parses, fine-grained error handling.

```aether
// Parser lifecycle
expat.parser_new()              -> ptr               // null on OOM
expat.parser_free(p)            -> void

// User-data slot — caller-allocated struct threaded through every
// handler invocation as the first arg.
expat.set_user_data(p, ud)      -> void
expat.get_user_data(p)          -> ptr

// Handler registration. The handler MUST be a bare named function
// cast at the call site via `as fn(...)`:
//     expat.on_start(p, my_handler as fn(ptr, string, ptr) -> void)
expat.on_start(p, cb: ptr)      -> void   // cb signature: fn(ud, name, atts) -> void
expat.on_end(p, cb: ptr)        -> void   // cb signature: fn(ud, name)       -> void
expat.on_text(p, cb: ptr)       -> void   // cb signature: fn(ud, text, len)  -> void

// Drive the parser
expat.parse(p, buf, len, is_final)   -> int   // 1 success, 0 parse error
expat.parse_full(p, buf, len)        -> int   // convenience: is_final=1

// Error reporting (call after a 0 from parse)
expat.error_string(p)           -> string   // null when no error
expat.error_line(p)             -> int
expat.error_column(p)           -> int

// Attribute walking (inside an on_start handler)
expat.attr_count(atts)          -> int      // number of name/value PAIRS
expat.attr_name(atts, i)        -> string
expat.attr_value(atts, i)       -> string
```

## Closure builder veneer (`parse_with`)

This is the recommended DX for most callers. The raw surface (next
section) is a 1:1 binding of libexpat's C API and demands a manual
user-data struct for per-parse state. The closure builder cuts all
of that boilerplate: per-parse state lives as ordinary Aether
locals, the closures capture them by reference, and a C-side
dispatcher routes libexpat's events to the right closure.

### Surface

```aether
// One-shot builder. _ctx is the auto-injected map from the
// trailing-block DSL — never reference it explicitly; just call
// the bind_* setters inside the block.
expat.parse_with(input: string) { /* bind_* calls */ }    -> int

// Setters used inside the parse_with trailing block.
// Closure signatures match the SAX events 1:1 — and ONLY the
// SAX-event args. No env / user-data param.
bind_start(|name: string, atts: ptr|        { ... })
bind_end(  |name: string|                    { ... })
bind_text( |text: string, len: int|          { ... })
```

Returns `1` on success, `0` on parse error. For error details on a
0 return, the v1 builder doesn't expose `error_string` etc. (the
parser is owned and freed internally). If you need the error
location, drop down to the raw surface for that parse.

### How it works (one paragraph)

`parse_with` allocates a libexpat parser plus a small C "handler
set" struct that holds up to three boxed closures (start / end /
text). Each `bind_*` inside the trailing block boxes its closure
and stashes the pointer into the handler set under a known key.
After the block runs, the builder body installs three C
trampolines on the parser (one per event); those trampolines
unbox the appropriate closure and call its `.fn` with its own
`.env`. libexpat itself has a single `user_data` slot — the
handler set fans it out to the three independent closure envs.
Closures' env memory is owned by the enclosing Aether scope
(heap-promoted captures), so the captured locals stay valid for
the entire synchronous run.

### Gotcha — closure params: SAX event args ONLY

The closure parameter lists in the `bind_*` setters MUST contain
exactly the SAX-event parameters — and nothing else. Specifically,
do NOT declare a leading `env` / `ud` / `user_data` parameter.

```aether
// CORRECT
bind_text(|text: string, len: int| { bytes = bytes + len })

// WRONG — extra `env: ptr` parameter
bind_text(|env: ptr, text: string, len: int| { bytes = bytes + len })
//        ^^^^^^^^^ shifts every following param by one slot;
//                  `len` reads uninitialised stack data, you get
//                  garbage numbers like 516052160 instead of 7.
```

Why: Aether's closure lowering prepends an implicit `_env` slot
(carrying captured-variable storage) to the closure's hoisted C
function. The C trampoline calls the closure with `(env_ptr, …SAX
args)`, where `env_ptr` lands in that implicit `_env` slot. If you
also declare an `env` parameter, it consumes the first SAX arg
position, and every subsequent declared param reads one slot off.
Captured variables still work (they go through `_env`), so test 1
above APPEARS to pass when only counting starts — but any closure
that actually uses a SAX-event arg (the `len` byte count, the `text`
pointer, the `atts` array) reads garbage.

Closure signatures by event:

| Event | Aether closure signature |
|-------|--------------------------|
| start | `|name: string, atts: ptr|` |
| end   | `|name: string|` |
| text  | `|text: string, len: int|` |

### Mixing handler subsets

Registering only some of `bind_start` / `bind_end` / `bind_text`
is fine — `xml_parser_install_handlerset` only wires libexpat
callbacks for the events whose closure was set. Skip the ones you
don't need.

### Lifetime and aliasing inside closures

- `name` (string in start / end events) is **borrowed** —
  libexpat owns the buffer. Don't store the raw string past the
  closure call. Copy with `string.from_bytes(name, string.length(name))`
  if you need to retain it.
- `text` (in text events) is **uncopied and unterminated** —
  same as the raw-API contract. Use the explicit `len` parameter,
  not `string.length(text)`. Copy into a strbuilder if you need
  the full run; libexpat may chunk text content across multiple
  callbacks at internal-buffer boundaries.
- `atts` (in start events) is libexpat's NULL-terminated alternating
  name/value array, only valid for the duration of the start
  callback. Walk it with `expat.attr_count(atts)` /
  `expat.attr_name(atts, i)` / `expat.attr_value(atts, i)`.
- Captured Aether locals (`count`, `bytes`, a strbuilder you
  declared in the enclosing scope) are heap-promoted and live for
  the entire synchronous `parse_with` call. Mutate them freely.

### Example — closure builder, one handler

```aether
import contrib.parsers.xml_expat

main() {
    count = 0
    expat.parse_with("<root><a/><b/><c/></root>") {
        bind_start(|name: string, atts: ptr| {
            count = count + 1
        })
    }
    println("element count = ${count}")   // 4 — root + a + b + c
}
```

No `Counter` struct, no `malloc`, no `set_user_data`, no `as fn` cast,
no top-level handler function. The closure captures `count` directly
from `main`'s scope.

### Example — closure builder, all three events

```aether
import contrib.parsers.xml_expat

main() {
    starts = 0
    ends = 0
    bytes = 0
    ok = expat.parse_with("<g>hi<inner/>there</g>") {
        bind_start(|name: string, atts: ptr| { starts = starts + 1 })
        bind_end(  |name: string|             { ends = ends + 1 })
        bind_text( |text: string, len: int|   { bytes = bytes + len })
    }
    println("ok=${ok} starts=${starts} ends=${ends} bytes=${bytes}")
    // ok=1 starts=2 ends=2 bytes=7
}
```

### Example — raw bare-fn surface (fallback / multi-parse use)

The same element-counting task using the raw SAX API. Use this when
you need long-lived parsers, multi-chunk streaming, or want to share
one handler function across multiple parses.

```aether
import contrib.parsers.xml_expat
import std.string

extern malloc(n: int) -> ptr
extern free(p: ptr)

struct Counter { n: int }

on_element(ud: ptr, name: string, atts: ptr) {
    c = ud as *Counter
    c.n = c.n + 1
}

main() {
    p = expat.parser_new()
    defer expat.parser_free(p)

    c = malloc(4) as *Counter
    c.n = 0
    defer free(c)
    expat.set_user_data(p, c)

    expat.on_start(p, on_element as fn(ptr, string, ptr) -> void)

    doc = "<root><a/><b/><c/></root>"
    if expat.parse_full(p, doc, string.length(doc)) != 1 {
        println("parse error: ${expat.error_string(p)}")
        exit(1)
    }
    println("element count = ${c.n}")    // 4
}
```

Note the differences vs. the closure builder: explicit struct +
malloc/free, `set_user_data` to thread the struct through, `as fn(...)`
cast to materialise the C function pointer, top-level `on_element`
function (must be a bare named function, not a closure).

## Callback contract

SAX handlers must be **bare named functions**. The Aether-side
registration uses `expr as fn(args) -> void` at the call site to
materialise a raw C function pointer, which libexpat invokes
directly with the same call-shape conventions as a C SAX handler:

- **Start handler**: `(void* user_data, const char* name, const char** atts)`.
  Aether: `fn(ud: ptr, name: string, atts: ptr)`. `atts` is libexpat's
  NULL-terminated alternating name/value array; walk it with
  `expat.attr_count(atts)` + `expat.attr_name(atts, i)` +
  `expat.attr_value(atts, i)`.
- **End handler**: `(void* user_data, const char* name)`. Aether:
  `fn(ud: ptr, name: string)`.
- **Text handler**: `(void* user_data, const char* text, int len)`.
  Aether: `fn(ud: ptr, text: ptr, len: int)`. The `text` pointer is
  an **uncopied, unterminated** slice of libexpat's internal buffer
  — copy what you need before the next callback fires. libexpat may
  also chunk text content across multiple callbacks at
  internal-buffer boundaries; concatenate via a strbuilder if you
  need the full run as one string.

For closure-based handlers (the `parse_with { ... }` builder
described earlier), the closure parameter list uses the same
SAX-event args — but **omits** the user-data parameter entirely.
See the "Gotcha" subsection above for details on why an explicit
`env` parameter in a `bind_*` closure miscompiles silently.

## Streaming (multi-chunk) parse

```aether
p = expat.parser_new()
defer expat.parser_free(p)
expat.on_start(p, on_element as fn(ptr, string, ptr) -> void)

while !eof {
    chunk, n = read_some_bytes(...)
    if expat.parse(p, chunk, n, 0) != 1 {           // is_final = 0
        bail_with_error(expat.error_string(p))
    }
}
// Final flush
expat.parse(p, "", 0, 1)                            // is_final = 1
```

## Not in v1

- **Namespaces / `xmlns`**: libexpat supports `XML_ParserCreateNS`
  for namespace-aware parsing; this wrapper uses `XML_ParserCreate`
  for simplicity. Add an `expat.parser_new_ns(sep_char)` variant if
  needed.
- **Tree (DOM) construction**: this is a streaming SAX surface only.
  A `parse_to_tree` convenience could be built on top using
  user-data to thread a builder; out of scope for v1.
- **Comment / processing-instruction / CDATA-section handlers**:
  libexpat exposes these via `XML_SetCommentHandler` etc.; can be
  added as additional registration helpers if downstream needs.
- **External-entity / DTD handlers**: same; libexpat-supported but
  not wired here.
- **Unicode / wide-char build**: we link the byte-oriented libexpat
  (`-lexpat`), not the wide variant (`-lexpatw`). All Aether
  strings are UTF-8 bytes end-to-end, matching the byte-libexpat
  convention.

## See also

- libexpat documentation: <https://libexpat.github.io/>
- `tests/integration/contrib_xml_expat/probe.ae` — full 8-case
  test matrix: the raw surface (lifecycle, handler registration,
  user-data round-trip, attribute walking, error reporting,
  multi-chunk parse, two-parser independence) plus the closure
  builder (single-handler capture, three-handler fan-out,
  missing-handler graceful path).
- `contrib/sqlite/README.md` — same contrib-tier build pattern.
