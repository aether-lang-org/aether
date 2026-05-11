# `ae help` demo — offline diagnostics for closure-DSL config scripts

`ae help <script.ae>` is the operator-friendly companion to `ae build`
and `ae run`. The typer's terse `error[E0301]: Undefined function 'super_token'`
becomes a concrete suggestion:

```
[1] undefined function at broken_dsl.ae:24:9
    Undefined function 'superuser_tokn'
       24 |         superuser_tokn("admin-1234")
                    ^
    help: Did you mean: superuser_token (in my_lib)?
    see:  docs/module-system-design.md (Selective import / exports)
```

## Files in this directory

| File | What it shows |
|------|---------------|
| [`broken_dsl.ae`](broken_dsl.ae) | A deliberately-broken closure-DSL with the three mistakes operators most often make: YAML-style `port: 9990`, HCL-style `host = "127.0.0.1"`, and a Levenshtein-distance typo on a setter name. |
| [`fixed_dsl.ae`](fixed_dsl.ae) | What `broken_dsl.ae` looks like after applying every suggestion. The diff is the lesson. |

## Try it

```bash
# Default mode: human-readable findings.
ae help broken_dsl.ae

# Machine-readable JSON (CI integration, structured logs).
ae help broken_dsl.ae --json

# Apply the safe rewrites — prints a unified diff, asks for
# confirmation, then writes atomically.  Refuses on non-TTY stdin
# so CI can't accidentally rewrite files.
ae help broken_dsl.ae --fix

# Compare your fixed version against the canonical one.
diff -u broken_dsl.ae fixed_dsl.ae
```

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
