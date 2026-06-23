# `std.http` client improvement plan

> **Status: v2 + v2.1 complete (commits on branch
> `feat/std-http-v2-client`).** Every gap in the "Why subversion can't
> use v1 today" table is closed; all seven test cases from the
> "Testing surface to add upstream" section live in
> `tests/integration/test_http_client_v2.ae` and pass. v3 streaming
> remains intentionally deferred per this document.
>
> Most thoroughly stress-tested by the Servirtium record/replay work,
> which has since moved out of the Aether stdlib (it used to ship as
> `std.http.server.vcr`) to its own repository,
> `github.com/aether-lang-org/servirtium-vcr`. See
> [`docs/http-vcr.md`](../http-vcr.md) for where it went and why.


`std.http` v1 client is a great starting shape: `http.get(url) -> (body, err)`
plus `post`/`put`/`delete` with a `(url, body, content_type)` signature. It's
enough to talk to a handful of "no-auth, JSON-in/JSON-out, 200 means good"
APIs, which is the right minimum for a stdlib.

This document captures what a v2 needs to look like for a real downstream
user — the Aether port of Apache Subversion at
`~/scm/subversion/subversion/` — to drop its hand-written
`ae/ra/shim.c` (1,417 lines of libcurl) and switch to `std.http`.

The proposed changes are additive. The v1 surface stays as-is; v2
introduces a builder-shaped request/response that supports headers,
status discrimination, and response-header capture.

The structure of this plan deliberately mirrors
`sqlite-improvement-plan.md`: v1 enables demos, v2 enables the
subversion port, v3 is sugar/optional.

## Why subversion can't use v1 today

The subversion RA shim issues every kind of HTTP call against an
authenticated REST server. The flows are:

- **Authenticated GET** — every request carries `X-Svnae-User: alice` and
  optionally `X-Svnae-Superuser: <token>` so the server can apply ACLs.
- **Status discrimination** — a path-not-found is `404`, an ACL-denied
  read is `403`, a missing auth token is `401`, anything else is `5xx`.
  Each is a distinct caller-visible outcome (different exit code in the
  CLI, different error path on commit).
- **Response-header capture** — `GET /repo/r1/path/foo.c` returns the
  blob's content-hash in `X-Svnae-Node-Hash`, which the verify path
  re-checks against the body. Without header access there's no integrity
  check.
- **POST with auth + body** — every commit, branch-create, and copy
  flows through `POST` with a JSON body and the same auth headers.

Mapping these to v1's `http.get(url) -> (body, err)`:

| Need                          | v1 supports? | Why not                                              |
|-------------------------------|:------------:|------------------------------------------------------|
| Set request headers           | ❌           | `get(url)` takes only the URL                        |
| Read response headers         | ❌           | wrappers free the response before returning          |
| Distinguish 200 / 404 / 403   | ❌           | non-2xx collapses to `"http error"`                  |
| Read raw status code          | ❌           | only "ok or err"                                     |
| Configurable timeout          | ❌           | hard-coded inside the C extern                       |
| Bytes-rather-than-string body | ⚠️           | `get` returns a `string` — fine for JSON, suspect for blobs |

The `extern http_response_*` accessors expose enough that a v2 wrapper
can be built without C-side changes. The gap is in the Aether-side
wrapper API; the underlying C is already capable.

The full call-site survey lives in `~/scm/subversion/subversion/ae/ra/shim.c`
(grep for `curl_easy_setopt` — every option is a v2 requirement).

## What the existing C externs already give us

Reading `std/http/module.ae`'s extern block shows the runtime side
already exposes more than the v1 wrappers expose to the language:

```ae
extern http_response_status(response: ptr) -> int           // 200, 404, ...
extern http_response_body(response: ptr) -> string
extern http_response_headers(response: ptr) -> string       // raw text dump
extern http_response_error(response: ptr) -> string
extern http_response_ok(response: ptr) -> int
```

So the v2 plan is largely about **publishing a wrapper API that exposes
the existing externs cleanly** plus adding a request-header path on the
input side.

## v2: builder-shaped request, full response access

The minimum surface to unblock subversion. Note that the request
builder takes the HTTP method as an arbitrary string — not an enum
of `{GET, POST, PUT, DELETE}`. That's a deliberate choice, not just
laziness; see §"Arbitrary HTTP methods" below for the case.

```ae
// ---- request builder ----
//
// Build up a request, fire it, get back a response handle the caller
// owns and frees. This replaces `get/post/put/delete`'s "url+body in,
// body+err out" shape — that's good for one-liners but doesn't carry
// headers in either direction.
//
//   req = http.request("GET", url)
//   http.request_set_header(req, "X-Svnae-User", "alice")
//   http.request_set_timeout(req, 30)
//   resp, err = http.send(req)
//   if err != "" { ... transport-level failure ... }
//   status = http.response_status(resp)
//   body   = http.response_body(resp)
//   hash   = http.response_header(resp, "X-Svnae-Node-Hash")
//   http.response_free(resp)
//   http.request_free(req)

request(method: string, url: string) -> ptr
request_set_header (req: ptr, name: string, value: string) -> string
request_set_body   (req: ptr, body: string, content_type: string) -> string
request_set_timeout(req: ptr, seconds: int) -> string
request_free       (req: ptr)

send(req: ptr) -> {
    response = http_send_raw(req)
    if response == null {
        return null, "out of memory"
    }
    err = http_response_error(response)
    if err != "" {
        err_copy = string_concat(err, "")
        http_response_free(response)
        return null, err_copy
    }
    return response, ""
}

// Caller drives status interpretation — non-2xx is no longer an error.
response_status (resp: ptr) -> int                                 // 200, 404, ...
response_body   (resp: ptr) -> string
response_header (resp: ptr, name: string) -> string                // case-insensitive lookup; "" if absent
response_free   (resp: ptr)
```

### Required new C externs

The wrappers above need three new C-side raw functions to back them:

```c
// In std/http/aether_http.c (or a new aether_http_request.c).

// Build an opaque request handle.
void* http_request_raw(const char* method, const char* url);

// Append a "Name: value" header to the in-progress request.
// Returns 0 on success, non-zero on failure (out of memory).
int   http_request_set_header_raw(void* req, const char* name, const char* value);

// Set request body bytes + length + content-type. Replaces any prior body.
int   http_request_set_body_raw(void* req, const char* body, int len, const char* content_type);

// Set per-request timeout in seconds. 0 = use default.
int   http_request_set_timeout_raw(void* req, int seconds);

// Free the request handle.
void  http_request_free_raw(void* req);

// Fire the request. Returns a response handle (NULL on transport failure).
// The caller frees with the existing http_response_free().
void* http_send_raw(void* req);

// Case-insensitive response-header lookup. Returns "" when the
// header is absent. Existing http_response_headers() returns the
// whole raw block; this one is the typed accessor every caller
// actually wants.
const char* http_response_header_raw(void* resp, const char* name);
```

Implementation notes:

- The request handle is a small struct holding (method, URL, header
  list, body bytes, body len, content type, timeout). `http_send_raw`
  translates it into the libcurl call sequence the wrappers used to
  bake into v1.
- The libcurl backend already supports custom headers (`CURLOPT_HTTPHEADER`),
  arbitrary methods (`CURLOPT_CUSTOMREQUEST`), explicit body (`CURLOPT_POSTFIELDS` +
  `CURLOPT_POSTFIELDSIZE` for binary safety), per-request timeouts
  (`CURLOPT_TIMEOUT`), and a header callback (`CURLOPT_HEADERFUNCTION`).
  None of v2's needs require new libcurl features — they're all already
  configurable, just not exposed.
- `response_header(resp, name)` should be case-insensitive — HTTP header
  names are case-insensitive on the wire, and forcing callers to know
  whether a server returned `X-Svnae-Node-Hash` or `x-svnae-node-hash`
  is needless friction.
- The existing `http.get` / `http.post` / `http.put` / `http.delete`
  one-liners stay. They're now expressible as thin wrappers over v2,
  which is a nice consistency check on the v2 design.

### Behaviour preserved from v1

- The `ok-or-error` Go-style return on `send()` covers transport-level
  failures (DNS, connect timeout, TLS handshake). Application-level
  failures (non-2xx HTTP status) are now the caller's call — `send()`
  returns the response cleanly and the caller decides what to do with
  status 404.
- `response_body` continues to return a `string` (AetherString-backed,
  binary-safe via `aether_string_data` / `aether_string_length`).
  Callers that know they're getting JSON treat it as text; callers
  with binary blobs reach for the data/length pair.

## Arbitrary HTTP methods

The request builder takes `method: string` rather than a closed set
like `{GET, POST, PUT, DELETE}`. This matters even though common-case
APIs only use the four classics.

HTTP/1.1 (RFC 7231 §4.1) defines `method` as an opaque token; new
methods are routinely defined by extension specs and are valid as
soon as both endpoints understand them. Real-world examples that
plausible Aether downstream users may need to send:

- WebDAV (RFC 4918): `PROPFIND`, `PROPPATCH`, `MKCOL`, `COPY`, `MOVE`,
  `LOCK`, `UNLOCK`. CalDAV / CardDAV / Microsoft Exchange Web Services
  all use these.
- DeltaV (RFC 3253): `REPORT`, `CHECKOUT`, `CHECKIN`, `MERGE`,
  `MKACTIVITY`. Used by some CMS / version-control servers.
- HTTP PATCH (RFC 5789): `PATCH` is now common in REST APIs (GitHub,
  GitLab, Stripe, ...). Already widespread enough that an enum that
  forgets it ages badly.
- WebSocket-adjacent / Server-Sent Events: not method-typed but often
  use custom headers a v1 design that hard-codes content-types
  alongside methods would also struggle with.
- Internal RPC dialects: some backends define project-specific verbs
  (`SUBSCRIBE`, `NOTIFY`, etc. — RFC 3265 / SIP-over-HTTP, custom
  IoT protocols).

A closed-enum API forces every one of these into a workaround:
either ship a parallel `request_with_method(...)` extern (de facto
re-introducing the string-typed form) or wait for a stdlib release
that adds the missing token. String-typed from the start avoids the
churn.

The cost of taking `method: string` is essentially zero — libcurl's
`CURLOPT_CUSTOMREQUEST` already takes a string, and the wrapper just
forwards it. The implementation should validate that the method is a
non-empty token of `[A-Z][A-Z0-9_-]*` (HTTP grammar) and reject
control chars / whitespace to prevent header smuggling, but otherwise
not gatekeep.

**Subversion port note:** the current Aether-side svnserver speaks a
greenfield JSON-REST API and only uses `GET` + `POST`. We have no
requirement to interoperate with reference Subversion's HTTP/WebDAV
dialect. Arbitrary-method support is on the v2 list because it's the
right design for `std.http`'s long-term shape, not because any
specific subversion-port code needs it today.

## v2.1: ergonomic helpers (optional)

Once v2 lands, every existing v1 entry point can be expressed as a
thin sugar wrapper, and the same shape extends to common new patterns:

```ae
// "GET this JSON URL with these auth headers."
get_json_with_headers(url: string, headers: ptr) -> {
    req = request("GET", url)
    /* loop over headers, calling request_set_header */
    resp, err = send(req)
    request_free(req)
    if err != "" { return "", 0, err }
    body = response_body(resp)
    status = response_status(resp)
    response_free(resp)
    return body, status, ""
}

// "POST this body and return the body + status."
post_with_status(url: string, body: string, ct: string) -> { string, int, string }
```

These are pure Aether — no new C externs. They can land as a v2.1
follow-up PR or be deferred to userland.

## v3: streaming bodies (later)

For very large response bodies (multi-megabyte file downloads), the
"materialise the whole body into one AetherString" model that v2
inherits from v1 stops scaling. A streaming form like:

```ae
// Drive the body chunk-by-chunk, never holding the whole payload.
stream(req: ptr, on_chunk: fn(string, int)) -> {
    rc = http_stream_raw(req, /* callback wiring */)
    return rc, ...
}
```

is straightforward to add on top of v2's request builder. Not a blocker
for the subversion port (no individual blob is large enough to matter
yet) and the shape benefits from real downstream pressure before being
fixed in stdlib. Defer until a user actually needs it.

## What's intentionally NOT in v2

- **Connection pooling / keep-alive.** libcurl handles this transparently
  via `curl_share_*`; for a stdlib v2 the request-per-call model is
  enough. Add a `http.client(...)` reusable handle later if profiling
  shows the per-request connect overhead matters.
- **Multipart form-data uploads.** Subversion's commit POSTs a single
  JSON body; nobody in the test suite needs `multipart/form-data`. Out
  of scope until a real user shows up.
- **Cookies / sessions.** Session affinity is server-side state; clients
  carry it via headers (and v2 supports custom headers).
- **Proxy / TLS-cert configuration.** All the libcurl options are there
  in C; expose them through `request_set_option(req, key, value)` later
  if needed. Subversion runs against `localhost` in tests and isn't
  blocked.

## Migration shape on the downstream side

Once v2 ships, the subversion port can:

1. Replace `ae/ra/shim.c`'s 1,417 lines of libcurl + buffer-grow + header
   callback + auth helper with maybe ~300 lines of Aether in
   `ae/ra/client.ae`. The packed-string parsers (`aether_ra_parse_log`,
   etc.) stay as-is — they don't touch HTTP.
2. Drop `-lcurl` from `aether.toml` `[build] link_flags`.
3. The `svnae_ra_set_user` / `svnae_ra_set_superuser_token` functions
   become Aether-side state (a struct cached at module level).

The greenfield JSON-REST API the Aether-side svnserver speaks today
needs only `GET` + `POST` from v2's method surface. The remaining
v2 features (custom request headers, response-header lookup, status
discrimination) are the load-bearing ones for this migration.

Net win: ~1,400 lines of C deleted, libcurl dependency gone from the
project, the auth + header + status logic readable as Aether instead
of buried in callbacks.

## Testing surface to add upstream

The existing `std.http` test covers v1 (get/post/put/delete one-liners
against a local mock server). v2 needs:

- `request_set_header` round-trip — a mock server that echoes
  `X-Test-Foo` back, asserts the header arrived.
- `response_header` lookup — same mock returning `X-Server-Tag: alpha`,
  asserts `response_header(resp, "x-server-tag")` matches (case-insensitive).
- `response_status` discrimination — 200, 404, 403, 500 each round-trip
  through `send()` without being collapsed to an error.
- `request_set_body` with binary content (embedded NULs) — POST a
  10-byte payload with a NUL in the middle, server asserts byte-exact
  receipt.
- `request_set_timeout` enforcement — server sleeps 2s, client sets
  timeout to 0.5s, send returns a timeout error.
- `send()` transport failure — wrong port, refused connection,
  asserts non-empty `err` and `null` response handle.
- v1 wrappers still work — `http.get(url)` round-trips a 200 OK body
  through the new internals.

Seven cases fit in one Bash test under `tests/integration/http_client_v2/`
following the same probe-server shape the existing v1 test uses.

## Summary

| Version | Surface added                                                        | Unblocks                                |
|---------|----------------------------------------------------------------------|-----------------------------------------|
| v1      | `get` / `post` / `put` / `delete` one-liners                         | demos, simple no-auth APIs              |
| **v2**  | **request builder + headers + status + response-header lookup**      | **subversion port (~1,400 LOC removed)** |
| v2.1    | sugar wrappers (auth-aware get/post, status-returning variants)      | smaller call sites                      |
| v3      | streaming response body                                              | very-large-payload downloads            |

The big lift is v2. Everything after is sugar.

## Context

This plan emerged from rounds 38–45 of the Aether port of Apache
Subversion. The port currently runs on a hand-written
`ae/ra/shim.c` that exposes the v2 surface above on top of libcurl;
it's well-shaken-out, used by every server-touching test in the suite,
and serves as a reference implementation for what `std.http` v2 should
look like. Happy to upstream it once the design is settled.

Companion to `sqlite-improvement-plan.md` — same shape, same downstream
project, same "v1 enables demos / v2 enables real users" framing.
