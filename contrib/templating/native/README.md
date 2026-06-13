# `contrib.templating.native` — escape-correct HTML emission

Templating as plain Aether code, no template DSL. The output format
and the escape rules live in the helper functions, not in the
template syntax — so an HTML template author physically cannot
produce un-escaped user content in an attribute.

This is the inverse of [`contrib.templating.liquid`](../liquid/),
which exists for the "operator brings their own `.liquid` file" case.
This module is for the case where the library author exposes a
template-shaped surface where users write *Aether*.

## Two surfaces

### Builder DSL (the templates-that-ARE-Aether-code pitch)

```aether
import contrib.templating.native

items = ["alpha", "beta", "<gamma>"]

out = native.render_html() {
    native.tag("ul") {
        for i in 0..3 {
            native.tag("li") { native.text(items[i]) }
        }
    }
}
// out == "<ul><li>alpha</li><li>beta</li><li>&lt;gamma&gt;</li></ul>"
```

The `for` is a *real Aether `for` loop*, not a re-implementation
inside a templating engine. Same with `if`, `switch`, `${...}`
interpolation, ref-cells, closures — everything you'd write
anywhere else in Aether is in scope and behaves identically. The
only DSL bits are `render_html`, `tag`, `text`, and `raw`.

Surface:

```aether
render_html() { ... }            // returns the rendered string
tag(name) { ... }                // emit <name>BODY</name>; BODY is the block
text(s)                          // append HTML-escaped (5 entities)
raw(s)                           // append verbatim — trusted markup only
```

**Note the parentheses on `render_html()` and `tag(...)`.** Aether's
trailing-block syntax attaches a block to a call site only when the
call has parentheses — `render_html { ... }` (no parens) parses as
"function reference, then unrelated block" and is a silent typing
error. `render_html() { ... }` parses correctly.

### XML emitter (skinny v1)

```aether
feed = native.render_xml() {
    native.xml_tag("rss") {
        native.xml_tag("channel") {
            native.xml_tag("title") { native.xml_text("Aether news") }
            for i in 0..items_n {
                native.xml_tag("item") {
                    native.xml_tag("title") { native.xml_text(titles[i]) }
                }
            }
        }
    }
}
// feed == "<?xml version=\"1.0\" encoding=\"UTF-8\"?><rss>...</rss>"
```

`render_xml()` emits the standard XML 1.0 / UTF-8 prolog and then
runs the trailing block against a fresh emitter. `xml_tag(name)
{...}` wraps a block in `<name>BODY</name>`. `xml_self_close(name)`
emits `<name/>`. `xml_text(s)` escapes the five canonical XML
entities (note: `'` → `&apos;`, NOT `&#39;` like HTML; `>` is also
escaped, more conservative than the strict spec but matches
`std.xml`'s writer).

Surface:

```aether
render_xml() { ... }             // prolog + rendered block
xml_tag(name) { ... }            // <name>BODY</name>
xml_self_close(name)             // <name/>
xml_text(s)                      // append XML-escaped
xml_raw(s)                       // append verbatim (CDATA, vetted markup)
```

**Skinny v1 has no attribute helper.** Attribute-shaped data either
lifts into child elements (`<item><name>x</name></item>` rather than
`<item name="x"/>`) or uses `xml_raw` to write a hand-built tag.
Adding `xml_attr` requires either (a) attributes-as-map on the tag
call (`xml_tag(name, attrs: m) { ... }`) or (b) a "pending tag" state
machine where `xml_attr` flushes the `>` on first non-attr call —
each has trade-offs and neither has a real user yet. We'll add it
once the first downstream user tells us which call-site shape they
need.

Also skipped in v1: namespaces (pass `xml_tag("svg:rect")` if you
already have the prefix, but `xmlns:` declaration support waits for
attributes), processing instructions other than the prolog, DOCTYPE,
schema-aware emission. Use `xml_raw` for trusted markup if you need
any of these.

### Pretty-print (debug-only)

Both `render_html()` and `render_xml()` emit tight output — no
indentation, no inter-element whitespace, byte-deterministic. For
human reading or diffing during development, two post-processors
take that tight output and return an indented version:

```aether
tight = native.render_html() {
    native.tag("ul") {
        native.tag("li") { native.text("a") }
        native.tag("li") { native.text("b") }
    }
}
println(native.pretty_print_html(tight, 2))
//   <ul>
//     <li>a</li>
//     <li>b</li>
//   </ul>

println(native.pretty_print_xml(some_xml, 4))     // 4-space indent
```

Surface:

```aether
pretty_print_html(s: string, indent_size: int) -> string
pretty_print_xml(s: string, indent_size: int)  -> string
```

`indent_size` is 2 or 4 in normal use; any value in `1..8` is
honoured, anything out of range falls back to 2. Line endings are
always `\n` — pretty-print is for humans, not machines.

**Caveats — read these before using pretty-print in production:**

- **It's for debugging, not serving.** The tight output is the
  ground truth; pretty-print is a transform layer for human eyes.
  Production paths should use `render_html()` / `render_xml()`
  directly.
- **HTML carve-outs.** Inside `<pre>`, `<textarea>`, `<script>`,
  and `<style>`, depth tracking pauses and the body passes through
  byte-for-byte. These four elements have whitespace-significant
  content and indenting inside them would change the rendered
  page.
- **XML has no whitelist.** Every XML element gets indented. If
  your XML uses mixed content (text and elements interleaved
  inside the same parent) or carries CDATA-sensitive data, the
  pretty-printed output is *not equivalent* to the tight version
  for the consuming parser. Use the tight renderer for such cases.
- **Not a general formatter.** The post-processor understands tags
  this module emits — `<name>`, `</name>`, `<name/>`, plus a
  fall-through for `<?...?>`, `<!--...-->`, and `<![CDATA[...]]>`.
  Arbitrary HTML / XML with attributes (today) or unusual entity
  use will be formatted on a best-effort basis. For arbitrary
  input, reach for `xmllint --format`, `tidy`, or your IDE.
- **Attributes (when they land).** The v2 XML attribute story
  hasn't shipped; when it does, the parser here will need updating
  to skip `key="value"` runs inside `<`...`>`. Until then, treat
  attribute-bearing markup as undefined for the formatter.

### Plain functions (escape hatch for hot paths)

If you want to avoid the per-tag strbuilder allocation the DSL
performs (every `tag()` creates a temporary buffer for its body),
drop down to the function-call surface and pass the strbuilder
explicitly:

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

Surface:

```aether
html_text(sb: ptr, s: string)             // append, HTML-escaped
html_raw(sb: ptr, s: string)              // append verbatim — trusted markup only
html_tag_open(sb: ptr, name: string)      // <name>
html_tag_close(sb: ptr, name: string)     // </name>
html_tag(sb: ptr, name: string, body: string)  // <name>escaped-body</name>
```

The DSL is built on top of these — `tag()` ultimately writes
through `strbuilder.append`, and `text()` calls `html_text`. No
behavioural divergence between the two surfaces; they're the same
output, different ergonomic trade-offs.

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
