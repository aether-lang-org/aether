# Aether integration tests

Multi-file / multi-process tests. Each subdirectory is one test
fixture; the test runner discovers it automatically (no manual
registration in the Makefile beyond the prune list — see below).

## Layout

Two patterns are in use:

```
tests/integration/<topic>/
    server.ae                 # the program under test
    test_<topic>.sh           # shell driver — boots server.ae,
                              # exercises it (curl, python, etc.),
                              # asserts behaviour, cleans up
```

…or for a single self-contained fixture:

```
tests/integration/<topic>.ae  # one-file test, runs as `ae run`
                              # under the per-file harness
```

The shell driver is the right choice when the test needs a host
process (curl, python, openssl), a backing server, multiple
processes, or wants to assert exit codes / stdout / stderr beyond
what `ae run` exposes. The single-file `.ae` form is for
self-contained programs whose own assertions exit non-zero on
failure.

## How a new test gets discovered

The `make test-ae` target discovers tests automatically. The
per-file harness (the `.ae` driver) is constrained by the prune
list in `Makefile` so that fixtures with their own shell driver
don't get double-run. **If your new test is a fixture directory
with a `test_<topic>.sh`, append the directory to the prune list
in the `test-ae` recipe.** The line is one big `find -prune`
chain — search for `'tests/integration/<existing-test>/*' -prune`
and add yours next to a similar one.

## Categories

Most subdirectories cluster by topic:

| Prefix                  | What it tests |
|-------------------------|---------------|
| `http_server_*`         | HTTP server (TLS, h2, keep-alive, middleware, ops) |
| `http_*` / `http_real_ip`, `http_auth` | HTTP middleware + features |
| `http_client_dechunk`   | `std.http.client` de-chunks `Transfer-Encoding: chunked` |
| `aeocha_*`              | `contrib/aeocha` test framework |
| `emit_lib*`             | `--emit=lib` packaging + sandbox |
| `namespace_*`           | `std.namespace` cross-language hosting |
| `cryptography_*`        | `std.cryptography` (SHA-1, SHA-256) |
| `aether_string_*`       | `AetherString` FFI boundary |

Pick the matching prefix when adding a new test in an existing area.

## Conventions

- Shell drivers must `set -e`, clean up child processes in a
  `trap cleanup EXIT` block, and write all output to a `mktemp -d`
  staging directory rather than the source tree.
- Skip cleanly (`echo "  [SKIP] reason"; exit 0`) when an external
  dependency is missing (curl, python, openssl, libnghttp2).
- Server-startup deadlines should match other HTTP tests in this
  tree (15s for plain HTTP servers, 30s for TLS / h2-tls).
- No person names anywhere — describe behaviour, not who reported
  it.
