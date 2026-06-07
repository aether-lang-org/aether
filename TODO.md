# Aether — TODO

Long-running list of things to build / decide later. Short-lived
tasks live in PRs and the CHANGELOG; this file is for ideas with a
hint of design but no committed scope.

## `contrib.templating.dsl` — native, no-reflection templating

**Why:** Aether already has the closure-DSL pattern
(`docs/closures-and-builder-dsl.md`). A native templating engine
fits naturally on top: the template *is* a `.ae` closure, the output
format is chosen by which emitter the builder runs against.

The `contrib.templating.liquid` port is shipping first because the
ecosystem-interop case is more immediate (operators bring their own
`.liquid` files). The native DSL is the inverse pitch: library
authors expose a template-shaped surface where users write Aether,
not template syntax.

### Sketch

```aether
import contrib.templating.dsl

t = template(html) {
    h1   { text("Hello ${user}") }
    each items |it| {
        li { text(it.name) }
    }
}
println(render(t))
```

Swap the first arg to switch output:
- `template(html)` — escape-aware HTML
- `template(xml)`  — XML (CDATA-aware, namespace-prefix-aware)
- `template(json)` — JSON (commas, array/object dispatch)
- `template(sql)`  — SQL with placeholder binding (never raw
                     concatenation — `text(user_input)` produces
                     `?` + a side-channel parameter list)

The escape decision lives in the **emitter**, not the template. That
is the whole reason this shape is valuable in a sandboxed language —
the template author cannot accidentally produce raw user content in
an HTML attribute.

### Dispatch model — three options to pick between

When the time comes to build this, decide first:

1. **One namespace per emitter** — `import contrib.templating.dsl.html`
   brings `h1/text/each` bound to HTML escape rules. SQL importers
   get `select/where/text` bound to bind-parameter rules. Same
   surface symbol (`text`) means different things based on which
   module imported it. No runtime dispatch, sandbox-clean. Mixing
   HTML + SQL in one file requires aliasing.
2. **Single namespace, emitter chosen at `template()` call** —
   runtime dispatch off the active emitter on the context stack.
   More flexible, slightly more magical, risk of invisible
   escape-mode changes if emitters nest.
3. **Emitter-as-typed-receiver** — `html.h1 { html.text(x) }`.
   Most explicit, most verbose. Dead-simple sandbox story.

Leaning toward option 1 — it's the most Aether-shaped (matches how
`std.fs` / `std.http` / etc. work).

### Dependencies

- The existing closures-and-builder-dsl machinery, no additions.
- `std.strbuilder` for output accumulation.
- No reflection, no runtime type dispatch — same constraints as the
  Liquid port.

### Out of scope (for v1 of this when it lands)

- Partials / layouts (the `extends`/`block` pattern from Liquid).
- Cross-emitter composition (an HTML fragment embedded in a SQL
  string, say) — solve when there's a real use case.
- Streaming output. v1 builds a string; if a server author needs
  chunked output, that's a v2 ask.

### When to pick this up

After the Liquid port has been in `contrib/` long enough to validate
the value-tree + filter-registry shapes, and after at least one
downstream user has asked for the "templates that ARE Aether
code" pitch explicitly.

---

## `contrib.templating.liquid` — v0 landed, more layers to come

**Branch:** `feat/contrib-templating-liquid` (off main at `035e701`,
not pushed)

**File:** `contrib/templating/liquid/module.ae` — ~200 LoC, compiles,
**tested** (`tests/integration/liquid_basics/`, 7 cases all green).

### v0 scope (what works today)

- Plain text passthrough
- `{{ varname }}` single-segment variable lookup against a string→string
  context
- Whitespace trimming inside `{{ ... }}`
- Unbound variables render as empty string (Shopify Liquid behaviour)
- Unterminated `{{` returns a non-empty error

That's it. Everything below is deferred and must arrive WITH its own
tests in the same commit/PR (no skeleton tests, no stub directories;
tests live alongside working code only).

### History of this round

The original ~1215-LoC speculative draft was discarded. It hit
multiple structural issues (the `fn` keyword problem, then `as int` /
`as ptr` coercion issues, then almost certainly more). A 1200-line
parser written without smoke-testing as it grew is a sunk cost — the
right move was to start over with the absolute minimum, prove it
works end-to-end, and grow from there. The discarded draft was a
useful exploration but not the right artifact to ship.

The lesson encoded into next-session policy: **add one layer, write
its tests, get them green, then add the next layer**. No more
speculative parser-shaped drafts.

### Adjacent compiler issue worth filing

While doing this work I confirmed that `fn name(...) -> T { ... }` at
top level errors with E0100 in user/contrib code, but stdlib modules
(`std/uuid/module.ae:29`, `std/url/module.ae:38/46/56/66/127`) use this
shape and compile. The compiler should make `fn`-prefixed declarations
either work everywhere or nowhere; right now it depends on which build
path the file goes through. Worth filing as a separate GH issue —
NOT blocking templating work (contrib uses bare-name throughout).

### Next layer to add — and its tests

The next-up scope, with tests that must arrive in the same commit:

**Filters chain over the lookup:** `{{ name | upcase }}`,
`{{ name | downcase }}`, `{{ x | default:"y" }}`, chained
`{{ name | upcase | reverse }}`. Test cases:

- Single filter, then another single filter
- Chained: 2-deep, 3-deep
- Filter with one positional arg (`default`)
- Filter with multiple positional args (`replace:"a","b"`)
- Unknown filter passes through identity (Shopify behaviour)

That layer requires:
- An expression parser (atom + `|` chain)
- A filter dispatch (`upcase` / `downcase` / `default` / `replace` /
  `size` to start)
- The renderer to call the filter chain instead of `lookup`

The implementation will use `std.json` for the value type so a filter
that returns a number (`size`) is distinguishable from one returning a
string. The current string→string context grows into string→json-value.
That migration is part of this layer and should NOT preserve the
v0 string-only context — the v0 tests get rewritten to use the new
context shape if the API changes.

### Layers still to build, in dependency order

Each layer below is to be implemented in its own commit with its own
tests added to `tests/integration/liquid_*/` in the same commit.
Don't write the next layer until the previous layer's tests pass.

**1. Tag parser (`{% ... %}`)** — currently throws `"unsupported tag"`.
Per-tag work:

- [ ] `{% comment %}...{% endcomment %}` — simplest, just skip body
- [ ] `{% assign x = expr %}` — bind a name in the current scope
- [ ] `{% if cond %}` / `{% elsif cond %}` / `{% else %}` / `{% endif %}` —
      conditional with else-if chain. Needs:
      - condition evaluator (truthy/falsy, comparison ops `==` `!=` `>` `<` `>=` `<=` `contains`)
      - `and` / `or` connectives
      - Liquid's special falsy rules: `nil`, `false`, **empty string is truthy** in Liquid (different from many languages)
- [ ] `{% unless %}` / `{% endunless %}` — `if` with inverted condition
- [ ] `{% for x in coll %}...{% endfor %}` — iteration. Needs:
      - iterable = array / range / object (Liquid iterates object values)
      - `forloop.index` / `forloop.index0` / `forloop.first` / `forloop.last` /
        `forloop.length` / `forloop.rindex` / `forloop.rindex0` exposed in
        loop body
      - `limit:N` / `offset:N` / `reversed` modifiers on the `for` tag
      - range form: `{% for i in (1..10) %}`
- [ ] `{% break %}` / `{% continue %}` — `for`-body control flow
- [ ] `{% case x %}{% when v %}...{% else %}...{% endcase %}` — pattern match
- [ ] `{% capture name %}...{% endcapture %}` — render-block-to-string assigment
- [ ] `{% cycle 'a', 'b', 'c' %}` (optional in v1; common in tables)
- [ ] `{% increment x %}` / `{% decrement x %}` (optional in v1)

**Scope contract:** each tag adds an `AST_NODE_*` constant + a parser
branch in `parse_tokens` + a renderer branch in `render_node`. All keep
the same list-of-string-lists representation as the existing nodes; no
new infrastructure needed.

**2. Operator-bearing expressions for `if` conditions** — the current
`parse_filtered` only handles `atom (| filter)*`. Conditions need:

- [ ] `==` `!=` `>` `<` `>=` `<=` binary ops (Liquid's are left-associative,
      no precedence — `a and b or c` is `((a and b) or c)`)
- [ ] `and` `or` logical
- [ ] `contains` (string-in-string, item-in-array, key-in-object)
- [ ] Parenthesized grouping is NOT a Liquid thing (their grammar is
      flat) — confirm before adding

**3. Filter additions to reach Shopify parity** — drafted ~18, need ~12 more:

Pure (default-on):
- [ ] `replace_first` (only first occurrence)
- [ ] `remove` / `remove_first` (replace with "")
- [ ] `split:":"` → array
- [ ] `truncatewords` (truncate by word count)
- [ ] `newline_to_br` (`\n` → `<br />`)
- [ ] `strip_html` / `strip_newlines`
- [ ] `escape_once` (HTML-escape, but skip already-escaped entities)
- [ ] `url_decode`
- [ ] `slice` (substring or array slice)
- [ ] `sort` / `sort_natural` (case-insensitive sort)
- [ ] `uniq`
- [ ] `map:"prop"` (extract a property from each array element)
- [ ] `where:"prop","value"` (filter array)
- [ ] `concat:array2` (array concat — distinct from string append)
- [ ] `compact` (drop nils)
- [ ] `round` / `ceil` / `floor` / `abs` / `at_least` / `at_most`

World-touching (gated):
- [ ] `date` — full strftime semantics (not just identity). Needs
      `std.os.now_utc_iso8601` or a date-parsing primitive. Currently
      returns `"[date filter disabled — call allow_world_filters]"`
      when gated and an identity pass-through when allowed; needs real
      formatting.
- [ ] `asset_url` / `img_tag` (Shopify-specific; out of scope unless a
      user asks)

**4. Includes** (`{% include 'partial' %}`) — requires `std.fs`:

- [ ] File resolution: relative to a configured template-search root
- [ ] Engine config field: `template_root: string` (defaults to ".")
- [ ] Variable scoping: include sees the parent context by default;
      `{% include 'p' with name %}` and `{% include 'p', k: v %}` pass
      specific bindings
- [ ] Recursion limit (Liquid caps at depth 100 — match that)
- [ ] Cache parsed partials by absolute path so a partial used N times
      is parsed once
- [ ] **Gate via `--with=fs` when --emit=lib** — sandbox story: the
      contrib module imports `std.fs`, so a `--emit=lib` user without
      `--with=fs` cannot use the include path. `parse_string` /
      programmatic API stays available without `--with=fs`.

**5. Layouts** (`{% extends 'parent' %}` + `{% block name %}...{% endblock %}`):

- [ ] `extends` MUST be the first non-whitespace token in the template
      (validation + clear error)
- [ ] `block` declarations in the parent define overridable regions
- [ ] `block` declarations in the child override; `{{ block.super }}`
      inserts the parent's content (Liquid's spelling varies — confirm
      Shopify's vs Django-inspired forks)
- [ ] Recursive `extends` chain — child extends middle extends base
- [ ] Same caching as `include`

**6. Error reporting**:

- [ ] Currently `lex` returns one of `"unterminated delimiter"` or `""`.
      `parse_tokens` returns `"unsupported tag: <body>"`. Both should
      include source line numbers (the lexer captures line numbers per
      token; the parse path needs to thread them).
- [ ] Filter errors: unknown filter → identity (Shopify behaviour,
      matches the WIP). Filter raising an error (e.g. `divided_by:0`)
      currently silently returns null; should propagate as a render
      error tied to a source position.
- [ ] Render-time error tuple: `(string, string)` already in place.
      The "fail loudly on type errors" mode (Shopify's `strict` flag)
      is out of scope per the design decisions.

**7. Public surface gaps**:

- [ ] `parse_file(path)` is currently a stub; needs `std.fs.read` +
      include-search-root configuration
- [ ] `render_file(t, ctx, path)` for streaming output to a file
      (useful for static-site generators) — optional, may not ship in v1
- [ ] `template_free(t)` — currently no explicit free; the underlying
      lists own their string memory and the std.json values are
      orphaned. Audit memory ownership before shipping.
- [ ] `engine_set_template_root(eng, path)` for include search
- [ ] `engine_set_max_include_depth(eng, n)` (default 100)

**8. Documentation:**

- [ ] `contrib/templating/liquid/README.md` — install, usage,
      sandbox-with-`--with=fs` story, mapping from Shopify-Liquid
      reference docs to what's supported here (clear list of
      OUT-of-scope features), the struct-binding `to_value` adapter
      pattern with a worked example
- [ ] `CHANGELOG.md` entry under `[current]`
- [ ] `LLM.md` idiom (probably one line: "operator-supplied Liquid
      templates → `contrib.templating.liquid`")

### Test plan — cases to cover as each layer lands

Tests live in `tests/integration/liquid_*/` (each is a directory with
one `.sh` driver + one or more `.ae` programs + `.liquid` fixture
files where needed). Pattern follows existing tests like
`tests/integration/http_serve_static_range/`.

**Layer-by-layer cases below.** When a layer lands, its full case set
goes in with it. `liquid_basics/probe.ae` already covers Layer 5 (the
v0 render path) for the cases v0 supports. As filters / tags / etc.
land, either extend `liquid_basics/probe.ae` or add a new sibling
directory (`liquid_filters/`, `liquid_tags/`, etc.).

**Layer 1 — lexer (`lex(src)` returns tokens):**

A single Aether program that lexes representative sources and
asserts token counts + kinds + contents. Cases:

- [ ] Plain text → 1 TEXT token
- [ ] `Hello {{ x }}!` → TEXT, OUTPUT, TEXT
- [ ] `{% if x %}Y{% endif %}` → TAG, TEXT, TAG
- [ ] `{{- x -}}` whitespace control: surrounding whitespace stripped
- [ ] Unterminated `{{ x` returns the `unterminated delimiter` error
- [ ] Empty source → 0 tokens
- [ ] Source with only delimiters, no text → no TEXT tokens

**Layer 2 — expression parser (`parse_expr(body)` returns expr ADT):**

- [ ] String literal: `'foo'` and `"foo"` both parse as EXPR_LITERAL_STR
- [ ] Integer literal: `42` and `-7`
- [ ] Boolean literals: `true` / `false`
- [ ] Nil literals: `nil` / `null`
- [ ] Empty / blank: `empty` / `blank` (parse as empty string)
- [ ] Bare variable: `name` → EXPR_VAR with one segment
- [ ] Dotted: `user.address.city` → EXPR_VAR with three segments
- [ ] Bracket: `items[0]` → EXPR_VAR with two segments (`items`, `0`)
- [ ] String-bracket: `items['key']` → segments include the unquoted key
- [ ] Mixed: `users[0].name`
- [ ] Single filter: `x | upcase`
- [ ] Filter with args: `x | replace:"a","b"`
- [ ] Filter chain: `x | upcase | reverse | first`
- [ ] Expression serialize → deserialize round-trip identity

**Layer 3 — value resolution (`resolve_var` against std.json contexts):**

- [ ] Top-level key: `user` against `{user: ...}` returns the inner value
- [ ] Nested: `user.name` against `{user: {name: "alice"}}`
- [ ] Array index: `items[0]` against `{items: ["a","b"]}`
- [ ] String-keyed bracket: `items['x']` (object access via bracket)
- [ ] Missing key → null
- [ ] Out-of-bounds index → null
- [ ] `.size` on array: returns length
- [ ] `.size` on string: returns byte length
- [ ] `.size` on object: returns key count
- [ ] `.first` / `.last` on array
- [ ] Type mismatch (`.x` on a number) → null

**Layer 4 — filters (one test program per filter family):**

For each filter, a small fixture template + expected output. Cover at
minimum:

- [ ] `upcase` / `downcase` / `capitalize` / `strip` / `lstrip` / `rstrip`
- [ ] `escape` HTML-safety (`&` → `&amp;`, etc.)
- [ ] `url_encode` (space → `+`, RFC 3986 unreserved kept)
- [ ] `size` / `first` / `last` on array, string, object
- [ ] `join` with and without explicit separator
- [ ] `default` — true falsy cases: nil, false, "", `[]`, `{}`. NOT
      false: 0 (Liquid treats 0 as truthy — confirm vs Shopify)
- [ ] `replace` — multi-occurrence
- [ ] `append` / `prepend`
- [ ] `truncate` — boundary at exact length (no ellipsis), boundary at
      length+1, custom ellipsis arg
- [ ] `plus` / `minus` / `times` / `divided_by` / `modulo` — integer
      math; check Shopify's "divide-by-zero returns null" behaviour
- [ ] Unknown filter passes through (matches Shopify)
- [ ] World-touching `date` returns the gated string when not allowed;
      formats correctly when allowed (after the full impl lands)

**Layer 5 — end-to-end rendering:**

A handful of fixture `.liquid` files with expected `.txt` outputs, run
via a single shell script:

- [ ] Hello-world: `Hello {{ name }}!` + `name=alice` → `Hello alice!`
- [ ] Filter chain: `{{ user.name | upcase }}`
- [ ] Multiple outputs in one template
- [ ] Output of a nested-access value
- [ ] Output that produces an empty string (nil value)
- [ ] Whitespace control: `{{- x -}}` strips surrounding whitespace

**Layer 6 — tags (when they land):**

One fixture per tag:
- [ ] `{% if %}` true branch, false branch, else, elsif chain
- [ ] `{% if a == b %}` / `{% if a != b %}` / `{% if a > b %}` etc.
- [ ] `{% if a and b %}` / `{% if a or b %}` / `{% if s contains 'foo' %}`
- [ ] `{% unless %}` inverse
- [ ] `{% for %}` over array, object, range
- [ ] `forloop.index/first/last/length` exposed correctly
- [ ] `for ... limit:N offset:N reversed`
- [ ] `{% break %}` / `{% continue %}`
- [ ] `{% case %}` / `{% when %}` / `{% else %}`
- [ ] `{% capture name %}...{% endcapture %}` + `{{ name }}` use
- [ ] `{% assign x = ... %}` + use
- [ ] `{% comment %}` body invisible
- [ ] Nested tags (if-in-for, for-in-if, capture-of-if, etc.)

**Layer 7 — includes + layouts (when they land):**

These run only under `--with=fs`. Sandbox test verifies the negative
case (no `--with=fs` → engine errors clearly):

- [ ] Plain `{% include 'partial' %}` — partial sees parent context
- [ ] `{% include 'p' with name %}` — scoped binding
- [ ] `{% include 'p', k: v %}` — multiple bindings
- [ ] Missing partial → render error with the path
- [ ] Circular include → error (depth limit)
- [ ] `{% extends 'base' %}` + `{% block %}` — child overrides parent
- [ ] Chain: child → middle → base
- [ ] Missing parent → error
- [ ] No `--with=fs` under `--emit=lib` → import gate rejects the
      contrib module (this falls out of Aether's existing
      capability-gate machinery; add the test that asserts the error
      message points at the right opt-in)

**Layer 8 — error reporting:**

- [ ] Lexer: unterminated `{{` → error includes line number
- [ ] Parser: bad expression → error includes line + what was expected
- [ ] Render: unknown tag → identity (matches Shopify), DOES NOT panic
- [ ] Render: filter that errors (e.g. `divided_by:0`) returns null +
      doesn't crash subsequent renders

**Layer 9 — fuzz / robustness:**

- [ ] Empty template → empty string output
- [ ] Template that is only delimiters → empty string output
- [ ] Very long template (10000+ lines) — make sure linear-time lex
- [ ] Deeply-nested `for` (10+ levels) — make sure no stack overflow
      in the renderer
- [ ] Template with embedded NULs in TEXT — lexer should preserve
      (`string.char_at_n` is binary-safe; verify)

### Working policy for this module

- One layer at a time. Add the code, add the tests, run the tests,
  green, commit. Then start the next layer.
- No speculative drafts. The big-bang 1215-LoC attempt was a sunk cost.
- No placeholder tests. Tests live alongside working code.
- Rewrite tests freely when the underlying API changes. v0's tests
  will need rewriting when the context migrates from string→string to
  string→json-value; that's expected.

### Design decisions already locked in (don't re-litigate)

- **Value model:** `std.json` value tree. `to_value(s: *MyStruct) ->
  ptr` adapter pattern for struct binding, documented in README with a
  worked example.
- **Filters:** pure shipped enabled; world-touching (`date`,
  `asset_url`, ...) off by default, opt-in via
  `liquid.allow_world_filters(engine)`.
- **Scope:** Tier A + includes/layouts. Multi-range, custom drops,
  registers, strict mode, ifchanged, the Shopify e-commerce filters
  (`money`, `payment_terms`, etc.) explicitly OUT.
- **Approach:** vertical slice first (`{{ var | filter }}` end-to-end),
  then expand tag-by-tag. After Layer 6 (tags), add Layer 7
  (includes/layouts) in one go.
- **Build path:** pure Aether, no C source. Contrib install pattern
  follows `contrib/xml/expat/` — module.ae lives in
  `contrib/templating/liquid/`, `import contrib.templating.liquid`
  pulls it in; downstream user adds nothing to their aether.toml
  (because the engine doesn't depend on any C libraries).

## Other parked work

(Add as it surfaces.)
