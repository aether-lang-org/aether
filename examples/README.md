# Aether examples

Working `.ae` programs grouped by area. Every file here builds and
runs; `make examples` exercises all of them as part of CI.

## Layout

| Directory | What's there |
|-----------|---------------|
| [`basics/`](basics/) | Hello-world / variables / arrays / strings / control flow / I/O. Start here when learning Aether. |
| [`actors/`](actors/) | Actor patterns: ping-pong, pipelines, fan-out, message-with-arrays, supervision. |
| [`stdlib/`](stdlib/) | Stdlib feature demos: HTTP client + server, file I/O, JSON, processes, OS environment. The HTTP server demos cover plain HTTP/1.1, HTTP/2 (h2 + h2c via libnghttp2), TLS, and concurrent dispatch. |
| [`c-interop/`](c-interop/) | `extern` + `@c_callback` patterns — calling libc, calling user C, exporting Aether functions to C. |
| [`embedded-java/`](embedded-java/) | Hosting a JVM in-process via `contrib.host.java`. |
| [`packages/`](packages/) | `ae add host/user/repo` package consumption demos AND PATH-style `--lib` layering ([`lib-path-layering/`](packages/lib-path-layering/)). |
| [`ae-help-demo/`](ae-help-demo/) | `ae help <script>` offline diagnostics — a deliberately-broken closure-DSL paired with its `--fix`-corrected version. Demonstrates Levenshtein, YAML/HCL pattern detection, and the `--fix` / `--json` modes. |
| [`applications/`](applications/) | Larger end-to-end programs that combine several stdlib areas. |

## Running an example

```
ae run examples/basics/hello-world.ae
ae build examples/stdlib/http-server.ae -o /tmp/srv && /tmp/srv
```

The Makefile target `make examples` builds every file under this
tree; CI fails if any example stops building.

## Adding a new example

- Place it under the directory that matches its area. If none fits,
  open a new directory and update this README's table.
- Self-contained. An example should be runnable as a single
  `ae run` (or `ae build && execute`), with no external test
  fixtures — those belong under `tests/integration/`.
- Keep comments focused on what the example demonstrates and how
  to interact with it (curl commands, expected output).
- No person names in comments. Describe the use case, not the
  contributor.
