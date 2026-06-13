# `contrib/templating/` — escape-correct output emission

This directory holds modules that turn structured input into text in
a *format-specific way*, where the surrounding format imposes escape
rules on the user-supplied data being interpolated. Two siblings
today:

- [**`native/`**](native/) — Aether-as-template-language. The library
  author exposes a closure-friendly surface where users write
  *Aether code*; the format and the escape rules live in the helper
  functions, not in template syntax. Trailing-block builder DSL plus
  a plain-function escape hatch. Today: HTML and XML.
- [**`liquid/`**](liquid/) — Shopify Liquid port. For operator-
  supplied templates (email bodies, dashboards, generated reports)
  where the template author is a trusted human but not a code
  author. Familiar syntax, sandbox-clean, deterministic.

## What "fits templating" actually means

The shape that fits this directory's purpose is:

> The format's primary unit is **delimiter + body + delimiter**, the
> body can contain user-supplied input that needs format-specific
> escape, and *escape is a property of a single value* — not
> influenced by what's around it.

HTML and XML pass cleanly. JSON, YAML, TOML and friends don't —
they're *value-tree* formats where the right encoding of a single
value depends on tree-level context (commas between elements, key
quoting depending on the value's own type, etc.), and the right
shape for them is "build a tree, then encode," not "escape and
splice."

The list below is the long answer to "could we add `<format>` to
`contrib.templating.native`?". When in doubt, the questions are:

1. Is the smallest output unit a paired `open + body + close`?
2. If I `escape_text("user input")` in isolation, is the result
   *always* safe regardless of where the caller splices it?
3. Does the user's input ever change the structure of the
   enclosing document (e.g. a newline breaking nesting)?

If (1) is yes, (2) is yes, and (3) is no, the format fits.

## Candidates that fit

### Shipped today

- **HTML** — the canonical use case. Five-entity escape (`&<>"'`),
  void elements (`<br>` no slash), no namespace handling.
  Server-side page rendering, email bodies for HTML-aware clients,
  generated reports.
- **XML** — sibling of HTML with XML's escape rules
  (`'` → `&apos;`, `>` also escaped for safety). Skinny v1 omits
  attributes, namespaces, processing instructions other than the
  prolog, and DOCTYPE. SOAP envelopes, sitemap, generic XML output.

### Future, after the v2 XML attribute story lands

These are all XML dialects. Once `xml_tag(name, attrs: ...)` exists
in some form, each is a small alias-style module that delegates to
`xml_*` and adds the format's fixed root element + required
children. None warrants being a primitive in `native/`; they'd live
as sibling modules under `contrib/templating/` or
`contrib/formats/`.

- **SVG** — XML with the `<svg xmlns="http://www.w3.org/2000/svg"
  ...>` root. Wraps `xml_tag` directly. Trivial once attributes
  exist.
- **RSS** — XML with fixed structure (`<rss version="2.0"><channel>`
  ...). Small module on top of `xml_tag`.
- **Atom** — XML feed format, sibling of RSS.
- **KML / GPX / MathML / TEI / DocBook** — all XML dialects, same
  story. Build them when someone asks; don't pre-empt.

### Borderline-fits — possible later, with caveats

- **LaTeX** — `\section{Name}` is delimiter-around-text and escape
  rules exist (`\` → `\\`, `_` → `\_`, `&` → `\&`, etc.). But text
  mode and math mode have *completely different* escape rules, and
  the wrong rules in the wrong mode silently break the document —
  the same "context-dependent escape" problem that disqualifies
  YAML. Possible to ship a text-mode-only version with explicit
  guardrails, but the failure mode is severe enough that we don't
  want to invite users to reach for it casually.
- **RTF** — brace-delimited groups, simple escape. Fits structurally
  but nobody emits RTF anymore. Skip without a user.

## Candidates that don't fit (and why)

### Value-tree formats — use a value-tree builder, not templating

These all want **"build a tree, call `encode()`"** as their shape.
Concatenating text with escape rules around it produces output that
looks correct but breaks on the first array-element separator or
context-dependent encoding choice.

- **JSON** — `std.json` already has the right shape (`json.obj()`,
  `json.arr()`, `json.str()`, `json.num()`, `json.encode()`).
  Re-implementing it under templating would be duplication and the
  `text + tag` model breaks on `,` between array elements. Use
  `std.json`.
- **YAML** — escape isn't local: multi-line user input can break
  the enclosing document's indentation, scalar style (plain /
  quoted / literal / folded) depends on the value's content, key
  encoding depends on whether the key has special characters. The
  right shape is a value-tree-then-serializer. None exists in
  std/contrib today; if you need YAML *output*, emit JSON
  (JSON is valid YAML 1.2 by spec) via `std.json`.
- **TOML** — like YAML, scalar encoding depends on type and
  surrounding section structure. Value-tree shape. No existing
  module; emit JSON if you need a config-shaped data format today.
- **INI** — section headers + key=value pairs, but value escape
  varies wildly between dialects (Python configparser, systemd,
  PHP). The "fits" surface would be a flat key-value writer, not
  templating-shaped.
- **Protobuf / MessagePack / CBOR / Avro / BSON** — binary
  serialization formats. Schema-driven, no escape model at all.
  Value-tree-then-encode; not templating.
- **S-expressions / EDN / Clojure-data** — value-tree shape.
- **CSV / TSV** — escape is value-local (each cell quotes its own
  embedded delimiter), but the format is flat — no nesting — so
  `tag { ... }` adds nothing over `csv_row([a, b, c])`. Belongs as
  a small flat serializer (`contrib/serializers/csv/` if/when
  someone needs it), not as a templating sibling.

### Wrong-tool formats — use the format's actual tool

- **SQL** — the right answer is **parameterised queries**: `?`
  placeholders in the query string + a side-channel list of bound
  values, where the database driver does the type-correct
  conversion. Templating SQL with text-and-escape is the most
  common injection vector in software. We deliberately do not
  provide a `sql_tag` in this module; it would teach the wrong
  instinct. Use `std.sqlite`'s prepared-statement API or the
  equivalent for your driver.
- **Shell / argv arrays** — argv is a list of strings the kernel
  hands to the child process; there is no template. `std.os.run`
  takes a `list` of arguments and quoting is handled by the OS
  spawn primitive. Templating-shaped `sh_text(s)` would invite
  command injection.
- **URL / query strings** — `std.url` has percent-encoding and
  query-parser support already, in a value-tree shape. Not a
  templating problem.
- **Email RFC 5322 / MIME** — too many context-dependent rules
  (folded headers, MIME boundary tokens varying per message,
  content-transfer-encoding chosen based on body bytes). Belongs in
  a real MIME-builder module that picks the encoding once it has
  the full message.

### Positional formats — not delimiter-shaped

- **Markdown** — most elements are *positional* (`# Heading` is a
  heading only at start-of-line; `**bold**` works only with no
  intervening newline; backtick fences must be balanced). The "tag"
  abstraction doesn't model any of this. Escape rules are also
  weak — different renderers accept different things. The right
  shape, if we ever build it, is a *renderer that knows the rules*,
  not a templating layer.
- **AsciiDoc / reStructuredText / Org-mode** — same positional-
  syntax story as Markdown.

## Sanity check before adding a new format

If someone proposes a new format under `contrib/templating/`,
the test is the three questions above. If any of them fails,
the right home is somewhere else:

| answer | right home |
| --- | --- |
| Smallest unit is `<tag>body</tag>` AND escape is value-local AND user input never reparents | `contrib/templating/<format>/` |
| Smallest unit is a value (string / number / object / array) | a value-tree builder module (`std.<format>` or `contrib/serializers/<format>/`) |
| Smallest unit is line-positional (start-of-line decides meaning) | a renderer module, not a templating module |
| The format has fundamentally context-dependent encoding (YAML scalar style, LaTeX math mode) | wait for a real user; ship guardrails if you must ship at all |
| The right answer is "use the format's parameter / prepared-statement API" | don't ship; document the right tool |

## See also

- [`docs/closures-and-builder-dsl.md`](../../docs/closures-and-builder-dsl.md)
  — design doc for the trailing-block builder pattern
  `native/` uses.
- [`std.json`](../../std/json/) — the value-tree serializer pattern
  that fits JSON and the other value-tree formats.
- [`std.xml`](../../std/xml/) — the SAX-style reader and quoting-
  correct writer in the standard library; `native/`'s XML emitter
  shares its escape conventions.
- [`contrib/parsers/xml_expat/`](../parsers/xml_expat/) — the
  libexpat SAX parser, the input-side counterpart to XML emission.
