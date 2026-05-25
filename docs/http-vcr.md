# `std.http.server.vcr` — Servirtium record/replay for HTTP tests

Aether's implementation of [Servirtium](https://servirtium.dev), a
cross-language HTTP record/replay framework. Tapes are markdown
documents — the same format Servirtium implementations in Java,
Kotlin, Python, Go, etc. use, so a tape recorded by any of them is
replayable here.

The metaphor: a VCR is the device, a tape is the medium, and
`record` / `replay` are the operations the device performs on the
medium.

## Why you'd use it

- **Test against a real API once, then forever offline.** First
  run records every request/response into a tape file. Subsequent
  runs replay from the tape — no network, no flakiness, no rate
  limits, no upstream-changed-overnight surprises.
- **Pin upstream behavior.** The tape becomes a contract: if the
  upstream service changes its response shape, the re-record checks
  catch the byte-level diff (`vcr.flush_or_check` or
  `vcr.flush_and_fail_if_changed`).
- **Run UI tests offline.** Static-content mounts let
  Selenium/Cypress/Playwright pull CSS/JS/images straight from
  disk without each one polluting the tape.
- **Scrub secrets before committing the tape.** Authorization
  tokens, session cookies, server-issued ids — register a
  redaction and the on-disk tape is clean while the in-memory
  capture (and thus the live SUT) sees the real bytes.

## Quick start (replay)

```aether
import std.http.server.vcr
import std.http
import std.http.client

extern http_server_stop(server: ptr)

// Use raw form: actor receive handlers can't cleanly discard the
// wrapper's string return without an unused-result warning.
extern http_server_start_raw(server: ptr) -> int

message StartVCR { raw: ptr }
actor VCRActor { state s = 0
    receive { StartVCR(raw) -> { s = raw; http_server_start_raw(raw) } } }

main() {
    raw = vcr.load("tests/tapes/my.tape", 18099)
    a = spawn(VCRActor())
    a ! StartVCR { raw: raw }
    sleep(500)

    // SUT now drives http://127.0.0.1:18099 — every call served
    // from the tape.
    body, err = http.get("http://127.0.0.1:18099/things/42")

    vcr.eject(raw)
}
```

## Quick start (record)

```aether
import std.http.server.vcr
import std.http
import std.http.client

main() {
    raw = vcr.load_record("tests/tapes/my.tape",
                          "https://supplier.example.test", 18100)
    http.server_start_background(raw)

    // The SUT/client library points at http://127.0.0.1:18100.
    // VCR forwards to the supplier and records the interaction.
    req = client.request("GET", "http://127.0.0.1:18100/things/42")
    resp, err = client.send_request(req)
    client.request_free(req)
    client.response_free(resp)

    // Servirtium-style drift check: stop the recorder, write the new
    // tape to the normal path, but return an error if it differs so
    // the test fails and git diff shows the change directly.
    http_server_stop(raw)
    ferr = vcr.flush_and_fail_if_changed("tests/tapes/my.tape")
}
```

## Tape format

Canonical Servirtium markdown. Each interaction is a
`## Interaction N:` heading followed by four `### ...` sections
(request headers, request body, response headers, response body).
The response body section's heading carries the status and
content-type:

````
## Interaction 0: GET /ok

### Request headers recorded for playback:

```
Host: 127.0.0.1:18091
```

### Request body recorded for playback ():

```
```

### Response headers recorded for playback:

```
Content-Type: text/plain
```

### Response body recorded for playback (200: text/plain):

```
ok-body
```
````

The triple-backtick fenced code blocks live inside the document —
they're part of the format, not markdown escaping.

## Surface (Aether-side, by feature)

| Feature                           | Functions                                   |
|-----------------------------------|---------------------------------------------|
| **Replay** (load + serve)         | `vcr.load`, `vcr.load_url` (tape fetched over HTTP), `vcr.eject`, `vcr.tape_length`  |
| **Record server**                 | `vcr.load_record`, `vcr.eject_record`       |
| **Direct capture + flush**        | `vcr.record`, `vcr.record_full`, `vcr.record_full_response`, `vcr.clear_recording`, `vcr.flush` |
| **Re-record checks**              | `vcr.flush_or_check`, `vcr.flush_and_fail_if_changed` |
| **Notes** (per-interaction)       | `vcr.note`                                  |
| **Redactions / unredactions**     | `vcr.redact`, `vcr.clear_redactions`, `vcr.unredact`, `vcr.clear_unredactions` |
| **Header removal**                | `vcr.remove_header`, `vcr.clear_header_removals` |
| **Strict request matching**       | `vcr.last_error`, `vcr.clear_last_error`, `vcr.last_kind`, `vcr.last_index`, `vcr.reset_cursor`, `vcr.set_strict_headers` |
| **Static-content mounts**         | `vcr.static_content`, `vcr.clear_static_content` |
| **Optional markdown formats**     | `vcr.emphasize_http_verbs`, `vcr.indent_code_blocks`, `vcr.clear_format_options` |

Field selectors for mutations: `vcr.FIELD_PATH`,
`vcr.FIELD_REQUEST_HEADERS`, `vcr.FIELD_REQUEST_BODY`,
`vcr.FIELD_RESPONSE_HEADERS`, `vcr.FIELD_RESPONSE_BODY`.

### Ignoring a header under strict matching

Strict matching (`vcr.set_strict_headers(1)`, or any tape with a
non-empty recorded request-headers block) compares the live request's
headers against the tape and rejects anything **unexpected** with a 599.
Real HTTP clients defeat this by attaching headers the tape never
recorded — Go `net/http`, `java.net.http`, Rust `ureq`, and .NET
`HttpClient` each send their own `User-Agent`, and some add `Accept`,
`Accept-Encoding`, or `Connection`.

The lever is `vcr.remove_header(vcr.FIELD_REQUEST_HEADERS, name)`: a
request-header removal is applied to **both** the recorded expectation
and the live request before they are compared, so naming a header there
tells strict matching to *ignore* it:

```aether
vcr.set_strict_headers(1)
vcr.remove_header(vcr.FIELD_REQUEST_HEADERS, "User-Agent")  // ignore it
raw = vcr.load("api.tape", 8080)
```

`Host` is already stripped during normalization, so it never needs this.
Embedding via the C ABI? `aether_vcr_embed_strict_ignore_common_headers()`
does the whole volatile set (`User-Agent`, `Accept`, `Accept-Encoding`,
`Connection`) in one call.

Gzip policy: record mode treats `Content-Encoding: gzip` as a
transport encoding. The caller receives the upstream gzip response,
but the tape stores the decoded body and omits `Content-Encoding` /
`Content-Length`. Playback serves decoded bodies by default and
restores gzip plus `Vary: Accept-Encoding` when the caller sends
`Accept-Encoding: gzip`.

## Servirtium roadmap status

The Servirtium project documents a 16-step methodical path for
new implementations at https://servirtium.dev. Aether's
`std.http.server.vcr` covers steps 1–12:

| Step | Feature                                       | Status |
|------|-----------------------------------------------|:------:|
| 1    | Climate API client + real-network tests       | ✓ |
| 2    | Replay via VCR                                | ✓ |
| 3    | Record-then-replay                            | ✓ |
| 4    | Re-record byte-diff check                     | ✓ |
| 5    | Multiple-interaction handling                 | ✓ |
| 6    | Library extracted to its own module           | ✓ |
| 7    | Other HTTP verbs                              | ✓ |
| 8    | Mutation operations                           | ✓ |
| 9    | Per-interaction Notes                         | ✓ |
| 10   | Strict request matching + last-error surface  | ✓ |
| 11   | Static-content serving                        | ✓ |
| 12   | Optional markdown format toggles              | ✓ |
| 13   | Publish to package land                       | — |
| 14   | Proxy server mode                             | — |
| 15   | Pass the cross-impl compatibility suite       | — |
| 16   | Cross-implementation interop                  | — |

The hostile-tape fixtures from the Servirtium project's
`broken_recordings/` are checked in under
`tests/integration/tapes/` and exercised by the strict-match
integration tests.

## What's next on the Servirtium list

The roadmap is methodical and pausable — a future contributor can
pick up exactly where steps 1–12 left off:

- **Step 13 — Publish to package land.** Package the VCR module
  under whatever Aether's distribution story turns out to be
  (`aether.toml` package metadata, registry publish flow). The
  module already lives in its own subtree
  (`std/http/server/vcr/`) with self-contained surface, so this
  is mostly packaging plumbing rather than redesign.
- **Step 14 — Proxy server mode.** A second mode of operation
  alongside record/replay: VCR sits *between* the SUT and a real
  upstream during normal operation (no tape involved), letting
  developers point browsers / Selenium / Postman at it for ad-hoc
  exploration. The recorder-side proxy plumbing from step 3 is
  a starting point — making it persistent rather than test-scoped
  is the new work.
- **Step 15 — Pass the cross-impl compatibility suite.** The
  Servirtium project maintains a canonical TCK that any
  conforming implementation must pass. The `broken_recordings/`
  fixtures are already absorbed into the strict-match tests; the
  full suite goes further (assertions about Notes positioning,
  redaction order, format-toggle interop, etc.).
- **Step 16 — Cross-implementation interop.** Hand a tape
  recorded by Java/Kotlin/Python Servirtium to Aether and confirm
  replay works (and vice versa). The format is shared by design,
  so the test is essentially an integration test against tapes
  from those other repos.

Steps 13 and 14 are the substantive ones. Steps 15 and 16 are
verification — if the implementation is right, they should pass
without further changes (and if they don't, the failures pinpoint
where this implementation drifted from the spec).

## When NOT to reach for VCR

- **You're testing the HTTP server itself.** VCR replaces the
  *upstream* in your test, not your server-under-test. For
  testing your own request handlers, use the in-process actor
  pattern: spin up an Aether HTTP server in an actor and drive
  client calls against it from the main thread (see
  `tests/integration/test_http_client_v2.ae` for a worked example).
- **The exchanges are inherently non-deterministic.** Tapes are a
  poor fit for endpoints that return server-current timestamps,
  unique request ids, or randomized payloads. Redactions can
  paper over a few of these; if it's most of the response, VCR
  isn't the right tool.

## Test coverage

| Test file                                              | What it exercises |
|--------------------------------------------------------|-------------------|
| `tests/integration/test_vcr_redactions.ae`             | Step 8 — flush-time scrubbing of body / path |
| `tests/integration/test_vcr_unredactions.ae`           | Step 8/10 — playback unredactions and header removals |
| `tests/integration/test_vcr_verbs.ae`                  | Step 7 — POST/PUT/HEAD/DELETE/OPTIONS/TRACE/PATCH plus GET-with-body |
| `tests/integration/test_vcr_drift_fail.ae`             | Step 4 — overwrite-and-fail drift behavior |
| `tests/integration/test_vcr_record_last_error.ae`      | Step 3/10 — record-mode diagnostics |
| `tests/integration/test_vcr_gzip_normalize.ae`         | Step 3 — gzip normalize/restore |
| `tests/integration/test_vcr_notes.ae`                  | Step 9 — per-interaction `[Note]` blocks |
| `tests/integration/test_vcr_strict_match.ae`           | Step 10 — header mismatch surfaces via `last_error` |
| `tests/integration/test_vcr_strict_match_body.ae`      | Step 10 — body mismatch surfaces via `last_error` |
| `tests/integration/test_vcr_strict_ignore_headers.ae`  | Step 10 — `remove_header(REQUEST_HEADERS, …)` makes strict matching ignore a stock client's `User-Agent` |
| `tests/integration/vcr_embed_abi/`                     | Embed C-ABI — disk + HTTP-sourced (`start_playback_url`) playback via `dlopen` |
| `tests/integration/test_vcr_static_content.ae`         | Step 11 — `/api/*` from tape, `/assets/*` from disk |
| `tests/integration/test_vcr_format_options.ae`         | Step 12 — `*GET*` + indented code blocks round-trip |
| `contrib/climate_http_tests/test_climate_real.ae`      | Live network — real WorldBank climate API |
| `contrib/climate_http_tests/test_climate_via_vcr.ae`   | Replay path — same 5 assertions, no network |
| `contrib/climate_http_tests/test_climate_record_then_replay.ae` | Record-then-replay loop, including `AETHER_VCR_RECORD` re-record check |

## See also

- [`docs/http-server.md`](http-server.md) — the inbound HTTP
  server (routes, middleware, TLS, h2, WS, SSE).
- [`docs/http-reverse-proxy.md`](http-reverse-proxy.md) —
  outbound forwarding to upstream HTTP servers (production
  proxy with pool / LB / health / cache / breaker).
- [Servirtium](https://servirtium.dev) — upstream project,
  including the canonical 16-step "starting a new
  implementation" guide and cross-language compatibility
  fixtures.
