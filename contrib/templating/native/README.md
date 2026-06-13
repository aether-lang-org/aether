# `contrib.templating.native` — escape-correct HTML emission (walking skeleton)

Templating as plain Aether code, no template DSL. The output format
and the escape rules live in the helper functions, not in the
template syntax — so an HTML template author physically cannot
produce un-escaped user content in an attribute.

This is the inverse of [`contrib.templating.liquid`](../liquid/),
which exists for the "operator brings their own `.liquid` file" case.
This module is for the case where the library author exposes a
template-shaped surface where users write *Aether*.

```aether
import contrib.templating.native as nt
import std.strbuilder

render_greeting(name: string) -> string {
    sb = strbuilder.new(64)
    nt.html_text(sb, "Hello ")
    nt.html_tag_open(sb, "b")
    nt.html_text(sb, name)         // auto-escapes — works even for `<script>`
    nt.html_tag_close(sb, "b")
    return strbuilder.finish(sb)
}

// render_greeting("Alice <script>")
//   == "Hello <b>Alice &lt;script&gt;</b>"
```

## v0 surface

```aether
html_text(sb: ptr, s: string)             // append, HTML-escaped
html_raw(sb: ptr, s: string)              // append verbatim — trusted markup only
html_tag_open(sb: ptr, name: string)      // <name>
html_tag_close(sb: ptr, name: string)     // </name>
html_tag(sb: ptr, name: string, body: string)  // <name>escaped-body</name>
```

The five canonical HTML entities (`& < > " '`) are escaped. Tag names
are NOT escaped — the markup-author writes `<div>`, user-data goes
through `html_text()`. Standard contract.

## Why this shape?

The walking skeleton intentionally stays with plain function calls
over a strbuilder rather than the trailing-block builder DSL
(`html { h1 { text(...) } }` with implicit `_ctx`, sketched in
[docs/closures-and-builder-dsl.md](../../../docs/closures-and-builder-dsl.md)).
The DSL is the eventual sugar; getting the escape contract right
first is the foundation. The two compose: when the DSL surface lands
it can layer on top of these helpers, because the escape rule lives
in `html_text`, not in syntax.

For control flow (`if`, `for`, `each` in Liquid terms), use Aether's
own — same `sb`, same escape contract:

```aether
for item in items {
    nt.html_tag_open(sb, "li")
    nt.html_text(sb, item)
    nt.html_tag_close(sb, "li")
}
```

## What's NOT supported

- XML / JSON / SQL emitters. Each would add its own
  `<fmt>_text` / `<fmt>_tag_*` triple with its own escape rule.
  Mechanical; deferred until a downstream user asks.
- Attribute helpers (`html_attr(sb, name, value)`). The current
  surface emits only bare `<name>` opens. If you need attributes:
  `html_raw(sb, " class=\"")`, `html_text(sb, css_class)`,
  `html_raw(sb, "\"")` — explicit and escape-correct, but verbose.
  A real `html_attr` helper is a v1 follow-up.
- The trailing-block builder DSL (`html { ... }`). When that lands,
  this module will gain a builder veneer that calls these helpers.

## When to use this vs. Liquid

- **Use `templating.native`** when *you* (a programmer) write the
  template, the inputs are statically typed, and you want the
  compiler to catch typos in variable names rather than the
  rendering engine to silently substitute empty strings.
- **Use `templating.liquid`** when an *operator* (a designer,
  marketer, sysadmin) writes the template — Liquid's flat syntax
  is friendlier to non-programmers and lets you reload the template
  at runtime without recompiling.

## Sandbox

The module imports only `std.strbuilder` and `std.string`. Neither
is capability-gated, so `contrib.templating.native` works under
`--emit=lib` with no `--with=` flags. No filesystem, no network, no
process spawn — by construction, since the surface is just
"append to a string".

## Testing

```
bash tests/integration/native_templating_skeleton/test_native_templating_skeleton.sh
```

## Status

Walking skeleton (v0). The shape may evolve before any v1 promise.
