#ifndef AETHER_HTTP_H
#define AETHER_HTTP_H

#include "../string/aether_string.h"
#include <stdint.h>

typedef struct {
    int status_code;
    AetherString* body;
    AetherString* headers;
    /* Transport-level failure: DNS resolution failed, TCP connect
     * refused, TLS handshake error, recv timeout, OOM, etc. When set,
     * status_code is 0 and body/headers/effective_url may be NULL.
     * Callers (and the v2 send_request wrapper) should treat a
     * non-empty error here as "the request didn't make it to a
     * useful response — discard the rest". */
    AetherString* error;
    /* Redirect-loop failure: the request DID get a usable response
     * (status_code is set to the last 3xx, body/headers populated)
     * but the chain couldn't reach a 2xx within the rules. Distinct
     * from `error` so callers that opt into automatic redirects via
     * set_follow_redirects() can still inspect the terminal 3xx
     * status / body to decide whether the chain failure is fatal.
     * Reasons the field gets populated:
     *   - "redirect hop limit reached"
     *   - "redirect loop detected (...)"
     *   - "redirect rejected: scheme downgrade (https → http)"
     *   - "malformed Location header"
     * The v2 send_request wrapper does NOT auto-free the response
     * when only `redirect_error` is set — `error` remains the
     * single signal for "no response is available". Issue #239. */
    AetherString* redirect_error;
    /* The URL that produced this response. For requests where redirects
     * were not followed (max_redirects == 0, the default), this equals
     * the URL the caller passed to http_send_raw. For requests that
     * followed redirects, this is the URL of the final hop — not the
     * original — so callers can disambiguate `client.response_url(r)`
     * vs the URL they originally passed to the builder. NULL until the
     * response is populated; readable via http_response_effective_url_raw. */
    AetherString* effective_url;
} HttpResponse;

// ---------------------------------------------------------------------------
// v1 one-liners — present from day one, kept callable for backward compat.
// Internally re-implemented as thin wrappers over the v2 builder below.
// They preserve the original behaviour of "no per-request timeout — block
// forever" by handing the v2 path a 0 timeout (the explicit "no timeout"
// sentinel).
// ---------------------------------------------------------------------------

HttpResponse* http_get_raw(const char* url);
// Same as http_get_raw but with a per-call timeout. timeout_ms is
// rounded up to whole seconds because the underlying SO_RCVTIMEO /
// SO_SNDTIMEO storage is integer seconds; pass 0 for "block forever"
// (matches http_get_raw's default).
HttpResponse* http_get_with_timeout_raw(const char* url, int timeout_ms);
HttpResponse* http_get_with_timeout_ns_raw(const char* url, int64_t timeout_ns);
HttpResponse* http_post_raw(const char* url, const char* body, const char* content_type);
HttpResponse* http_put_raw(const char* url, const char* body, const char* content_type);
HttpResponse* http_delete_raw(const char* url);
void http_response_free(HttpResponse* response);

// Response field accessors. All are NULL-safe: passing NULL or a freed
// response returns a sensible default (0 or "") rather than crashing.
// Returned const char* pointers from the `_str` / headers / error accessors
// are borrowed — owned by the response struct and valid only until
// http_response_free().
int http_response_status(HttpResponse* response);
// Returns an OWNED, retained AetherString (cast to const char*). Unlike the
// borrowed accessors, its lifetime is independent of the response: it survives
// http_response_free(), so reading the body after freeing the response is safe.
// C callers read content via aether_string_data() and must string_release() it
// (Aether callers get automatic release via the `@heap` extern annotation).
// This closes the response-body use-after-free footgun where a caller freed the
// response before reading the borrowed body (http-serve-and-dial-reentrancy-ask.md).
const char* http_response_body(HttpResponse* response);
/* Byte length of the response body — binary-safe accessor that
 * partners with `http_response_body` for callers that may receive
 * payloads with embedded NULs (gzip, protobuf, image formats).
 * Returns 0 when response or body is NULL. */
int  http_response_body_length(HttpResponse* response);
const char* http_response_headers(HttpResponse* response);
const char* http_response_error(HttpResponse* response);

// Convenience: returns 1 if the request succeeded (no transport error
// AND HTTP status is in the 2xx range), 0 otherwise. Use this for the
// common "did it work?" check instead of chaining error/status calls.
int http_response_ok(HttpResponse* response);

// Legacy accessor aliases kept for callers that used the older
// `_code` / `_str` names. Prefer the short names above.
int http_response_status_code(HttpResponse* response);
const char* http_response_body_str(HttpResponse* response);
const char* http_response_headers_str(HttpResponse* response);

// ---------------------------------------------------------------------------
// v2 client — request builder, full response access.
//
// Build a request with method + URL + headers + optional body + explicit
// timeout, fire it, get back the full HttpResponse with status / body /
// raw header block, plus a typed case-insensitive header lookup. The
// caller drives status interpretation — non-2xx is no longer collapsed
// to an error; only transport-level failures (DNS, connect, TLS handshake,
// timeout) populate response->error.
//
// Lifecycle:
//   req = http_request_raw("GET", "https://example.com/api/users");
//   http_request_set_header_raw(req, "Authorization", "Bearer ...");
//   http_request_set_timeout_raw(req, 30);   // seconds; 0 = block forever
//   resp = http_send_raw(req);
//   http_request_free_raw(req);
//   /* read resp via the existing http_response_* accessors */
//   http_response_free(resp);
//
// Naming: every v2 client extern carries an `http_request_` /
// `http_send_` / `http_response_header_` prefix that doesn't collide
// with the existing http_response_* accessors above OR with the
// server-side surface in aether_http_server.c (`http_server_*`,
// `http_request_body`, etc. — those stay flat for tinyweb-compat).
// ---------------------------------------------------------------------------

typedef struct HttpRequest HttpRequest;  /* opaque */

HttpRequest* http_request_raw(const char* method, const char* url);

// Returns 0 on success, non-zero on failure (NULL request, OOM,
// invalid header). Header names are stored verbatim and emitted as
// `Name: value\r\n`; built-in headers the wrapper would set itself
// (Host, Content-Length) are overridden by an explicit set_header
// with the same name. Multiple values for one name produce multiple
// `Name: value` lines (RFC 7230 §3.2.2 conformant).
int http_request_set_header_raw(HttpRequest* req, const char* name, const char* value);

// Set the request body. `len` is explicit so binary payloads with
// embedded NULs survive. content_type may be NULL (defaults to
// application/x-www-form-urlencoded for backward compat with v1).
// Replaces any prior body.
int http_request_set_body_raw(HttpRequest* req, const char* body, int len, const char* content_type);

// Per-request timeout in whole seconds (v1 surface). 0 means
// "no timeout — block forever". Negative values are an error.
// Internally multiplied to nanoseconds; prefer
// `http_request_set_timeout_ns_raw` for sub-second precision.
int http_request_set_timeout_raw(HttpRequest* req, int seconds);

// Per-request timeout as nanoseconds. 0 means "no timeout — block
// forever". Sub-second precision is preserved through to the socket
// layer: `select` uses tv_sec + tv_usec (microsecond resolution),
// `SO_RCVTIMEO`/`SO_SNDTIMEO` use `struct timeval` (microseconds) on
// POSIX or a DWORD millisecond count on Winsock. POSIX retains full
// μs; Winsock rounds up to the next whole millisecond so that a
// sub-ms value doesn't degrade to "infinite" via DWORD=0.
int http_request_set_timeout_ns_raw(HttpRequest* req, int64_t timeout_ns);

// Configure automatic redirect-following on this request. `max_hops` of
// 0 (the default) keeps the v1/v2 behaviour: redirects are returned as
// 30x to the caller, which decides what to do. `max_hops > 0` follows
// up to that many redirect responses; the loop stops when a non-3xx
// status comes back, when the hop limit is reached (returns the last
// 3xx response with an error string set), when a redirect points back
// to a URL we've already visited (loop detection), or when an HTTPS
// origin tries to redirect to HTTP (scheme downgrade rejection).
//
// Authorisation headers are not forwarded across host changes; the
// builder strips Authorization / Cookie / Proxy-Authorization when the
// redirect target's host differs from the previous host. Callers that
// need cross-host auth can re-`set_header(req, ...)` between sends.
//
// Negative values are an error.
int http_request_set_follow_redirects_raw(HttpRequest* req, int max_hops);

// Skip TLS peer + hostname verification for THIS request (curl -k /
// wget --no-check-certificate). `on` non-zero enables the skip; 0 (default)
// verifies. Relaxed per-SSL, never on the shared process-wide SSL_CTX, so an
// insecure request cannot downgrade verification for other requests.
int http_request_set_insecure_raw(HttpRequest* req, int on);

void http_request_free_raw(HttpRequest* req);

// Fire the configured request. Returns an HttpResponse on success
// (caller frees with http_response_free), NULL only on out-of-memory
// failures BEFORE the request is sent. Transport failures (DNS,
// connect, TLS, timeout) return a non-NULL response with the failure
// recorded in response->error and status_code == 0.
HttpResponse* http_send_raw(HttpRequest* req);

// Case-insensitive response-header lookup. Returns "" when the header
// isn't present. The pointer is owned by the response and valid until
// http_response_free(). Multiple values for one header are joined
// with ", " (RFC 7230 §3.2.2 conformant).
const char* http_response_header_raw(HttpResponse* response, const char* name);

// Returns the URL of the response — the original request URL when no
// redirects were followed, or the URL of the final hop when they
// were. Useful after `http_request_set_follow_redirects_raw(req, N)`
// to discover where the chain landed without re-parsing Location
// headers from response->headers. NULL/free-safe.
const char* http_response_effective_url_raw(HttpResponse* response);

// Returns the redirect-class error (hop-limit / loop / scheme-downgrade /
// malformed-Location) for a response produced by a request that opted
// into automatic redirect-following. Returns "" when the chain
// completed normally (or the request never opted in). Distinct from
// http_response_error which signals transport-level failures only.
// Issue #239.
const char* http_response_redirect_error_raw(HttpResponse* response);

#endif
