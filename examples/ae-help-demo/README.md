# `ae help` demo — offline diagnostics for closure-DSL config scripts

This directory has TWO scripts intentionally:

| File | What it is | Command to run |
|------|------------|----------------|
| [`broken_dsl.ae`](broken_dsl.ae) | A **deliberately broken** closure-DSL with three mistakes (YAML colon, HCL equals, Levenshtein typo). Will NOT compile. Its purpose is to be the input for `ae help`. | `ae help broken_dsl.ae` |
| [`fixed_dsl.ae`](fixed_dsl.ae) | The **runnable** version after applying every suggestion `ae help` made. Compiles and runs cleanly. | `ae run fixed_dsl.ae --lib .` |
| [`my_lib/`](my_lib/) | Tiny standalone closure-DSL library used by `fixed_dsl.ae`. Setters print what was configured; `serve` prints a banner. No real server, no network, no side effects beyond stdout. | (imported by `fixed_dsl.ae`) |

> The `--lib .` flag tells `ae` to look in the current directory for
> the `my_lib` module — that's the multi-entry `--lib` mechanism
> from issue #413 in action, layered into the `ae help` demo so you
> see both features at once.

## Quick tour

```bash
# 1. Run the broken script through ae help. You'll see the three
#    mistakes called out with concrete suggestions:
$ ae help broken_dsl.ae

ae help — 2 findings for broken_dsl.ae

[1] YAML-style colon at broken_dsl.ae:33:9
    port on line 33 looks like YAML; Aether setters use call form
      33 |         port: 9990
         |         ^
    help: Did you mean: port(9990)?
    (safe to auto-apply with --fix)
    see:  docs/closures-and-builder-dsl.md

[2] HCL-style equals at broken_dsl.ae:36:9
    host on line 36 looks like HCL; Aether setters use call form
      36 |         host = "127.0.0.1"
         |         ^
    help: Did you mean: host("127.0.0.1")?
    (safe to auto-apply with --fix)
    see:  docs/closures-and-builder-dsl.md

# 2. Auto-apply the safe rewrites. ae prints the unified diff and
#    asks for confirmation; --fix refuses on non-TTY stdin so CI
#    can't accidentally rewrite files.
$ ae help broken_dsl.ae --fix

# 3. After the YAML/HCL rewrites land, re-run `ae help` and the
#    Levenshtein-matched suggestion for the `superuser_tokn` typo
#    surfaces (it was masked before by the parser bailing on the
#    YAML syntax error):
$ ae help broken_dsl.ae
[1] undefined function at broken_dsl.ae:21:9
    Undefined function 'superuser_tokn'
    help: Did you mean: superuser_token (in my_lib)?

# 4. Or just compare to the canonical version:
$ diff -u broken_dsl.ae fixed_dsl.ae

# 5. Run the fixed version. Output:
#      port           = 9990
#      host           = 127.0.0.1
#      superuser_token= admin-1234
#      repo           = alpha -> /data/alpha
#      === my_lib starting up ===
$ ae run fixed_dsl.ae --lib .

# 6. Machine-readable findings (CI integration, structured logs):
$ ae help broken_dsl.ae --json
```

## Why does `ae run broken_dsl.ae` fail?

`broken_dsl.ae` is **supposed** to fail. It exists so you have a
concrete input to point `ae help` at. The typer would correctly
emit `error[E0100]: Expected statement in block` on the `port: 9990`
line — that's the precise, terse output a build pipeline wants. `ae
help` is the operator-friendly companion: it takes the same input
and produces the actionable suggestion.

To see the demo's positive output, run `ae run fixed_dsl.ae --lib .`
— that's the script that's meant to compile and run.

## Privacy contract

`ae help` runs **fully on-machine** — no network calls, no telemetry,
no file reads outside the script + its resolvable imports +
co-located `*.help.md` hint files. This matters for config-IS-code
files that routinely embed secrets (API keys, super-tokens, internal
hostnames); sending them to a remote LLM "for help interpreting the
error" leaks them. The contract is enforced both **structurally**
(no socket APIs linked in the default build) and at **runtime** via
an `strace -e network` guard in CI.

The optional `--llm <weights.gguf>` escalation links a local
`llama.cpp` shim; user supplies their own weights file; same
no-network contract. Default builds omit the LLM module entirely.

## See also

- [`docs/cic-help.md`](../../docs/cic-help.md) — full design
- [`docs/config-is-code.md`](../../docs/config-is-code.md) — the pattern this command serves
- [`docs/closures-and-builder-dsl.md`](../../docs/closures-and-builder-dsl.md) — the syntax the pattern matcher walks
- [`docs/module-system-design.md`](../../docs/module-system-design.md) — `--lib` search path
