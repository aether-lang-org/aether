# std.http.server.vcr TODO

Notes from scanning `/home/paul/scm/servirtium-README`, excluding the weather API walkthrough. Keep this file about the generic VCR/Servirtium implementation only.

## Current Shape

- Playback and record mode share the same in-memory tape storage and Servirtium markdown emitter/parser.
- Record mode listens like playback, forwards with `std.http.client`, records markdown, and writes to the same file path playback uses.
- Playback supports multiple interactions, custom HTTP methods, strict request header/body matching, notes, static-content bypass, indented code blocks, emphasized HTTP verbs, response headers, and base64-marked response bodies.
- Recording redactions apply at flush time to path, request headers, request body, response headers, and response body.
- Playback unredactions apply to path, request headers, and request body before request matching.
- Header removals apply to request/response header blocks at recording flush time, and to recorded/live request headers before playback strict matching.
- Route registration already accepts wildcard method/path fallback for both playback and record modes.

## Test Coverage Map

These tests are the long-term contract for the features above:

- Step 3 record mode: `tests/integration/test_vcr_redactions.ae`, `tests/integration/test_vcr_verbs.ae`
- Step 4 drift/write-out semantics: `tests/integration/test_vcr_drift_fail.ae`, plus `tests/integration/test_vcr_redactions.ae` for mutation-aware tape rewriting
- Step 7 verbs: `tests/integration/test_vcr_verbs.ae`
- Step 8 redactions and header removals: `tests/integration/test_vcr_redactions.ae`, `tests/integration/test_vcr_unredactions.ae`
- Step 9 notes: `tests/integration/test_vcr_notes.ae`
- Step 10 strict playback matching: `tests/integration/test_vcr_strict_match.ae`, `tests/integration/test_vcr_strict_match_body.ae`, `tests/integration/test_vcr_unredactions.ae`
- Step 11 static content bypass: `tests/integration/test_vcr_static_content.ae`
- Step 12 markdown formatting options: `tests/integration/test_vcr_format_options.ae`
- Step 15 compatibility-suite prep: no dedicated test yet
- Step 16 HTTP-sourced tapes: no dedicated test yet

## Header Removal Mutations

Delivered first cut:

```aether
vcr.remove_header(field, header_name) -> string
vcr.clear_header_removals()
```

Implementation notes:

- Header removal operates on canonical newline-delimited `Name: Value` blocks.
- Match header names case-insensitively.
- Avoid regex until the language/runtime has a standard regex story; exact header-name matching is enough for the first pass.
- Recording removals run in `emit_one_interaction()` before `emit_code_block()`.
- Playback-side request-header removal normalizes both the recorded expectation and the live request before comparing.

Tests:

- Recording flush removes `Authorization` from request headers.
- Recording flush removes `Set-Cookie` from response headers.
- Playback can ignore a live `X-Request-Id` header when configured.
- Removal does not mutate in-memory capture; clearing removals and flushing again restores original headers.

## Separate Mutation Phases

Servirtium step 8 distinguishes six mutation points:

- recorder: caller request before forwarding to upstream
- recorder: real response before recording
- recorder: caller request as written to tape
- recorder: real response as returned to caller
- playback: caller request before matching
- playback: recorded response before returning to caller

Current Aether VCR intentionally has a simpler model:

- recording redactions at flush time
- playback unredactions before request matching
- header removals at recording-flush and playback-match time

Potential next design:

```aether
const PHASE_RECORDING_TAPE = 1
const PHASE_PLAYBACK_MATCH = 2
const PHASE_RECORDING_UPSTREAM = 3
const PHASE_RECORDING_CALLER_RESPONSE = 4
const PHASE_PLAYBACK_CALLER_RESPONSE = 5
```

Do not add all phases unless there is a concrete downstream need. Header removal and response mutation are probably the first useful slices.

## Record-Mode Last Error

Servirtium step 3 says upstream failures in record mode should set the same last-error surface playback uses.

Current behavior:

- record dispatcher returns `502` / `500` with an explanatory body.
- record dispatcher updates `vcr.last_error()`, `vcr.last_kind()`, and `vcr.last_index()` on failures.
- record-mode failures report `KIND_RECORD_ERROR`; failed recordings use `last_index() == -1` because no interaction was accepted into the tape.
- playback mismatch updates `vcr.last_error()`, `vcr.last_kind()`, and `vcr.last_index()`.

Gap:

- Future slices may split `KIND_RECORD_ERROR` into more specific record-mode failure kinds if tests need to distinguish transport, build, OOM, or tape-append failures.

Tests:

- `tests/integration/test_vcr_record_last_error.ae`: record mode against a refused upstream returns 502 and `vcr.last_error()` mentions the upstream transport failure.
- `tests/integration/test_vcr_record_last_error.ae`: `clear_last_error()` clears record-mode errors too.

## Recording Drift Semantics

Servirtium step 4 says record mode should fail if the newly recorded markdown differs from the previous recording, while still writing the new markdown so developers can use `git diff`.

Current Aether VCR:

- `flush(tape_path)` overwrites the tape.
- `flush_or_check(tape_path)` writes a `.actual` sibling and returns an error when bytes differ.
- `flush_and_fail_if_changed(tape_path)` overwrites `tape_path` with the new recording and returns an error when bytes differ, so tests can fail while `git diff` shows the drift directly.

Tests:

- `tests/integration/test_vcr_drift_fail.ae`: first recording creates the tape without failure.
- `tests/integration/test_vcr_drift_fail.ae`: identical re-recording returns success and does not create `.actual`.
- `tests/integration/test_vcr_drift_fail.ae`: changed re-recording overwrites the tape and returns a mismatch error.

## Gzip Normalize/Restore

Servirtium step 3 says recordings should store uncompressed content but re-gzip for callers when needed.

Current behavior:

- Record mode treats `Content-Encoding: gzip` as a transport encoding.
- The caller still receives the upstream gzip response in record mode.
- The tape stores the decoded response body.
- The tape omits `Content-Encoding` and `Content-Length`.
- Playback serves the decoded body by default.
- Playback restores gzip and sets `Vary: Accept-Encoding` when the caller sends `Accept-Encoding: gzip`.

Tests:

- `tests/integration/test_vcr_gzip_normalize.ae`: tiny gzipped upstream payload records as readable tape body.
- `tests/integration/test_vcr_gzip_normalize.ae`: playback without `Accept-Encoding: gzip` returns decoded body.
- `tests/integration/test_vcr_gzip_normalize.ae`: playback with `Accept-Encoding: gzip` restores gzip headers.

Remaining gap:

- The first cut assumes decoded gzip bodies are textual enough for the current tape string storage. Full arbitrary binary decoded bodies should go through the existing `base64 below` path once the record-side emitter carries explicit body lengths end to end.

## HTTP-Sourced Markdown Tapes

Servirtium step 16 mentions markdown recordings sourced over HTTP.

Potential API:

```aether
vcr.load_url(tape_url, port) -> ptr
```

Implementation:

- Use `std.http.client` to fetch markdown.
- Parse the fetched string using the existing parser path, probably by extracting a `parse_tape_text(label, markdown)` helper from `parse_tape_file()`.
- Preserve filesystem `load()` as the default.

Tests:

- Local test server serves a tape markdown file.
- `vcr.load_url()` replays it.
- Transport failure returns null and sets/prints a useful load error.

## Compatibility Suite

Servirtium step 15 points to the TODO-backend compatibility suite.

This is a larger conformance target rather than a single API gap.

Useful preparation:

- Ensure record mode can capture all methods used by the suite. Wildcard method routing should already cover this.
- Ensure repeated response headers round-trip.
- Ensure request body matching is binary-safe enough for suite traffic.
- Add a small runner/shim only after core behavior is stable.

## Secondary: Embedded Server For Other Languages

> **First embedding slice landed (0.181.0):** `vcr_embed_abi_wish.md`
> (repo root) is DONE — the PIC runtime (`--emit=lib --with=net` links a
> `.so`), the port-0 `getsockname` fix + `http_server_port()`, and the
> `aether_vcr_embed_*` C-ABI (`std/http/server/vcr/embed.ae`), each with
> a C-driver acceptance test. servirtium-dotnet is the first consumer.
>
> **Record-mode chunked decoding fixed:**
> `vcr_record_chunked_dechunk_wish.md` is DONE — `std.http.client` now
> de-chunks `Transfer-Encoding: chunked` responses, so record tapes
> store (and replay serves) the decoded payload rather than chunk
> framing. Same shape as the gzip-normalize path below.

Long-horizon goal, probably many months out: make the Aether VCR usable as an embedded Servirtium server behind test-framework wrappers for higher-level languages. The primary product shape should stay server-first: the system under test talks HTTP to an Aether VCR server, and the language wrapper gives the test author a fixture object that exposes a base URL, lifecycle, tape configuration, mutations, and diagnostics.

Reference point:

- `/home/paul/scm/servirtium-java/` shows the kind of user-facing capability expected from a mature language binding.
- We do not need ABI compatibility with Servirtium-Java or any other existing implementation.
- We do need capability compatibility: record, playback, redaction/mutation, header removal, notes, drift detection, static bypass, strict matching, markdown interop, and diagnostics should all be available to wrapper authors.

Candidate wrapper targets:

- C#
- Ruby
- Python
- Go
- Node-side JavaScript

End-user shape to aim for:

- Test code uses a wrapper fixture, e.g. `Servirtium.playback(...).start()`.
- The application/client library under test knows only the supplier service base URL.
- Tape paths, record/playback mode, mutation rules, notes, and last-error checks live in test harness setup/teardown, not in the client library.
- Dynamic ports should be supported so parallel test runners can avoid collisions.
- Labels should be supported for diagnostics, e.g. a test name like `"cant go overdrawn if checking acct"`, but labels are not the state lookup key.

Native shape to aim for:

- A flattened external VCR library that is easy to link from FFI layers.
- The embedded Aether webserver is the default runtime model, not an implementation detail to hide.
- A small, stable C ABI over opaque server handles rather than exposing Aether internals.
- The native handle is the lifecycle/state key. Host/port is where the SUT connects. Label is human-facing context for errors/logs.
- Server-bound state: each active embedded server owns its tape, cursor, mutations, static mounts, pending note, and diagnostics.
- Explicit ownership rules for every returned string/buffer.
- Binary-safe request/response body APIs, not just NUL-terminated strings.
- Per-server state, not process-global tape/cursor/mutation state, once multiple simultaneous VCR servers become a real need.
- A conformance suite shared across wrappers so each language proves the same Servirtium markdown behavior.

Conceptual ABI sketch, not a commitment yet:

```c
typedef struct AetherVcrServer AetherVcrServer;

AetherVcrServer* aether_vcr_start_playback(
    const char* label,
    const char* tape_path,
    const char* host,
    int port);

AetherVcrServer* aether_vcr_start_record(
    const char* label,
    const char* tape_path,
    const char* upstream_base,
    const char* host,
    int port);

void aether_vcr_stop(AetherVcrServer* server);
int aether_vcr_port(AetherVcrServer* server);
char* aether_vcr_base_url(AetherVcrServer* server);
char* aether_vcr_last_error(AetherVcrServer* server);
void aether_vcr_free_string(char* s);
```

Initial implementation direction:

- Do not start other-language bindings yet.
- Keep the current `std/http/server/vcr` Aether API working.
- Document v1 embedding as "one active VCR server per process" if globals remain in place.
- When refactoring, group the current globals into a `VcrState` and hang that state from an eventual `AetherVcrServer` handle.
- Only add lower-level "feed native request/response events into the core" APIs if a wrapper genuinely needs to bypass the embedded Aether server.

Do this only after the core VCR behavior has settled. The present `std/http/server/vcr` implementation still has C globals and Aether-module boundaries that are fine for in-repo tests but not yet a clean embeddable server contract.

## Verb Coverage Smoke Tests

Servirtium step 7 explicitly calls out `POST`, `PUT`, `HEAD`, `DELETE`, `OPTIONS`, `TRACE`, and `PATCH`.

Current VCR route registration is method-agnostic, but the test coverage is still GET-heavy. Useful follow-up:

- Add direct record/playback smoke tests for at least one non-GET verb with a request body.
- Add one HEAD case to confirm the server's GET-fallback path does not break the tape matcher.
- Add one `OPTIONS` or `TRACE` case if the compatibility suite needs them later.

## Things To Keep Avoiding

- Do not implement record mode as an HTTP proxy unless explicitly adding the optional proxy mode from the Servirtium roadmap.
- Do not use CORS tricks for record mode.
- Keep `std/http/server/vcr` implementation comments generic; tests may mention specific downstream protocols/services, but implementation should not.
