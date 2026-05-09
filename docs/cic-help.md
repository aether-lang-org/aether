# `aether help <script>` — Offline diagnostics for config-IS-code

A proposed companion command to `aether run` and `aetherc`. Goal:
catch the common mistakes a less-experienced operator makes when
authoring a closure-DSL config script (`my_server.ae`-shaped files in
the [`config-is-code.md`](config-is-code.md) sense), and translate the
compiler's terse output into something an operator can act on.

**Status:** proposed; not implemented. This doc is the design sketch.

> Closely scoped: this is not a general "make my Aether code work"
> assistant. It targets the narrow band of mistakes people make when
> they're using a builder-DSL library someone else wrote — where the
> *grammar* is wrong but the *intent* is usually clear from context.

## Why a separate command

Two reasons the typer alone isn't enough:

1. The typer is a precision instrument. It says "Undefined function
   'super_token' at line 7" and stops. That's correct, minimal, and
   the right behaviour for a build tool. It is not, however, the
   signal an operator who's never written Aether before needs to
   unstick themselves.
2. The closest existing equivalent — feeding the error to a remote
   LLM service — is **off-limits** for this use case. Config-IS-code
   files routinely contain secrets: API keys, super-tokens, internal
   hostnames, paths into private mounts. Sending them to a third
   party for "help interpreting the error" leaks them. See [Privacy
   model](#privacy-model) below.

`aether help <script>` is a separate, opt-in tool with a different
operating point: forgiving, verbose, heuristic, and **fully
on-machine**.

## Privacy model

Hard requirements:

- **No network calls.** Not telemetry, not error reporting, not
  "anonymized" usage stats, not a remote LLM. The command runs the
  same offline as it does online.
- **No file reads outside the working set.** Reads the script, reads
  imports it can resolve (stdlib + user libs the script names), reads
  hint files (`*.help.md`) shipped alongside libraries the script
  imports. Doesn't traverse `$HOME`, doesn't probe the filesystem for
  unrelated files.
- **No execution of the script.** Static analysis only. Even a
  well-meaning "compile then run with stubbed externs to see what
  happens" path could fire `os.system` / `os.exec` calls embedded in
  the user's pre-flight setup.
- **Output is local stdout.** No log files, no caching of the script
  body to disk. Anything the tool prints, the user sees and can pipe
  themselves.

These are the same rules `aetherc` already follows for the build
path. `aether help` extends them to a diagnostic surface without
relaxing them.

## Command surface

```
aether help <script.ae>           # run all checks, print findings
aether help <script.ae> --fix     # apply safe rewrites in place,
                                  #   print a diff first
aether help <script.ae> --json    # machine-readable findings
```

The argument is required — `aether help` with no argument falls
through to the existing CLI usage banner. Disambiguation happens on
"is the next token a path that exists with `.ae` extension."

Alternative names considered: `aether explain`, `aether check`, `ae
lint`. `help` reads naturally given the form ("Aether, help me with
this script"). Final naming is a small bikeshed; the capability is
what matters.

## What it does

Each capability below is a static-analysis pass that reads only the
script and its declared imports. None require running the script.

### 1. Spelling-matched suggestions for unresolved symbols

For every "Undefined function `X`" the typer would emit, the help
pass collects the exported-name set of every imported library, runs
Levenshtein-1 / Levenshtein-2 matches, and surfaces the closest
candidates plus the full setter list:

```
Undefined function 'super_token' at line 7.
Did you mean: superuser_token
Available setters in avnserver: repo, port, superuser_token,
                                host, no_compress
```

Implementation: the typer already builds a symbol table per
translation unit. Reuse it, walk it, score candidates by edit
distance.

### 2. "I'm writing YAML / HCL" pattern detection

Inside any closure-DSL block (a trailing `{ ... }` after a
`builder`-keyword or fn-taking-fn callee), pattern-match for
non-Aether shapes:

| User wrote | Help suggests |
|---|---|
| `port: 9990` | `port(9990)` (Aether setters use call form) |
| `port = 9990` | `port(9990)` (HCL `=` not valid here) |
| `repos: [...]` | one `repo(...)` call per entry |
| `port("9990")` | `port(9990)` (drop quotes; setter expects int) |
| `- name: alpha` | `repo("alpha", "/path")` |

The list is bounded by what people actually do — keep it short and
high-precision. False positives are worse than misses here, since
the user is already confused.

### 3. Type-mismatch with English

When the typer reports "expected int, got string" on a setter call,
join the reported types to the parameter name from the library's
exported signature:

```
Line 6: port() expects an integer (the TCP port). You passed a
        string. Drop the quotes: port(9990) not port("9990").
```

### 4. Missing-import detection

For every "Undefined function `f`" not resolved by step 1, check the
stdlib export tables for any module exporting `f`. If exactly one
matches, suggest the import:

```
'env' is exported by std.os. Add this import near the top:
    import std.os (env)
```

If multiple match, list them; let the user choose.

### 5. Library-author-shipped hints (`*.help.md`)

Libraries opt into richer diagnostics by shipping a `<name>.help.md`
file alongside their `.ae` source. The format is structured
markdown:

```markdown
# avnserver — common authoring mistakes

## bind shadows libc

If your DSL block uses `bind(...)` as a setter name (or you wrote
your own helper named `bind`), the C linker will silently pick the
libc symbol and your listening socket will fail to attach. Use
`host(...)` instead — same intent, no collision.

Pattern: setter or function definition named `bind`, `listen`,
        `accept`, `connect`, `socket`, `select`.

## serve without repo

A `serve { ... }` block with no `repo(...)` call compiles cleanly
but `serve_opts` returns 2 ("no repo declared") at runtime.

Pattern: builder block where the callee is `avnserver.serve` and
        no `repo` call appears.

## --no-compress in production

avnserver.no_compress(1) (or `no_compress(1)` inside `serve { }`)
is a bench escape hatch; produced reps/ are 5-15x larger than
needed and aren't replicated to peers correctly. Don't ship to
prod.

Pattern: `no_compress(1)` call where the script doesn't also set
        `STAGE != "prod"` first.
```

`aether help` reads these (only for libraries the script imports),
matches each section's `Pattern:` clause against the AST, and
surfaces only the matching ones. The library author absorbs the
institutional knowledge once; every novice script gets the benefit
without the typer growing library-specific rules.

The pattern language can stay simple — start with literal-name
matches and AST-shape predicates; resist the urge to grow it into a
rule engine. If a library's hints need more than that, the library
should ship better docstrings on its setters instead.

### 6. Lint for "compiles but fails at runtime"

Some failures live below the typer. The classic for `serve`-shaped
libraries:

```
serve { ... } block at line 4 has port(9990) but no repo() call.
serve_opts will return 2 ("no repo declared") at runtime.
At least one repo("name", "/path") is required.
```

Surfaced via library-shipped hint files (above), so this stays
library-agnostic at the tool layer.

### 7. Top-level closure-DSL detection

A novice writes:

```aether
import avnserver
avnserver.serve { repo("a", "/x"); port(9990) }
```

The typer's complaint will be structural and confusing.
`aether help`:

```
Line 2: serve { ... } is at top-level. Aether scripts run main()
as the entry point. Wrap it:

    main() {
        avnserver.serve { repo("a", "/x"); port(9990) }
    }
```

### 8. Side-by-side canonical template

The library author can ship `_examples/serve.ae` (or whatever name
their hint file points at). On any failure, `aether help` prints
the user's broken script next to the canonical example. For someone
mid-transition from YAML, this is often more useful than the most
precise error message.

### 9. Cross-link to docs

Every help message ends with a doc reference:

```
See:  docs/config-is-code.md (Library author recipe)
      avnserver/embed.ae (canonical serve builder)
```

Like `man -k` for the closure-DSL.

### 10. `--fix` for the safe rewrites

For mechanical replacements where the intent is unambiguous:

- `super_token` → `superuser_token` (single-candidate Levenshtein-1
  on a closed export set)
- `port: 9990` → `port(9990)` (YAML colon → call form, when the
  RHS is a literal)
- `port("9990")` → `port(9990)` (quoted int literal where setter
  expects int)
- Missing imports inferred uniquely from undefined names

Always print the diff first; require explicit confirmation. The
goal is teach-by-correction, not a magic black box.

## What it does NOT do

These are deliberately out of scope:

- **No network.** No telemetry, no remote services, no LLM API.
- **No script execution.** Static analysis only.
- **No "ask an AI."** See the next section for the local-LLM
  escalation path, which is opt-in and still offline.
- **No general code-improvement suggestions** beyond the
  closure-DSL config use case. This is not a linter for arbitrary
  Aether — there are other tools for that.
- **No library-author rule DSL.** `*.help.md` patterns stay simple;
  if a library needs more expressive hints, the project's `.ae`
  source and docstrings are the right place.

## Optional escalation: local LLM (`--llm`)

Past a point, heuristic pattern-matching has diminishing returns.
A small on-device language model — llama.cpp-flavoured, a 3-7B
weights file the user supplies — could push the diagnostic quality
further by:

- Synthesising plain-English explanations for errors the heuristics
  caught but couldn't articulate well.
- Suggesting fixes for novel patterns the static rules missed.
- Translating typer output into the operator's first language.

This stays strictly opt-in:

```
aether help my_script.ae --llm /path/to/model.gguf
```

The flag does three things and only three things:

1. Loads the user-supplied weights file via an embedded llama.cpp
   shim. No download, no fetch.
2. Constructs a prompt from the static-analysis findings + the
   relevant lines of the script.
3. Streams the model's response to stdout.

No network. No "we'll cache your queries." If the user doesn't pass
`--llm`, the LLM code path doesn't run at all — the embedded shim
is gated behind a feature flag at compile time, so a stripped
distribution can omit the binary cost entirely.

The recommendation in the help command's own usage banner is plain:
heuristics are sufficient for ~80% of novice mistakes; the LLM
escalation is a sharp tool for the long tail, and you bring your
own weights.

## Implementation sketch

The pieces, roughly:

- **Hook into the typer.** Reuse the existing AST + symbol-table
  passes. Add a "diagnose" mode that collects findings instead of
  aborting on first error.
- **Hint loader.** Walk the imported libraries, find any
  `<name>.help.md` next to their `module.ae` / source, parse the
  structured sections.
- **Pattern matcher.** Run the per-pattern AST predicates. Keep this
  set small and high-precision — additions need a real-world repro,
  not just a hypothetical.
- **Renderer.** Pretty-print findings: line, original, suggestion,
  doc cross-ref, diff if applicable.
- **`--fix` applicator.** Take findings flagged "safe-fix," apply to
  source, write back atomically (write-then-rename), print
  before/after diff.

Total surface: comparable in size to a moderately-featured linter.
The `--llm` mode is an additive feature gate; the heuristic core
stands alone.

## Cross-references

- [`config-is-code.md`](config-is-code.md) — the pattern this command
  serves; the "library author recipe" section defines the surface
  `aether help` introspects.
- [`closures-and-builder-dsl.md`](closures-and-builder-dsl.md) — the
  syntax/semantics the help command's pattern matcher walks.
- [`getting-started.md`](getting-started.md) — where the help command
  itself should be cross-linked once it ships.
