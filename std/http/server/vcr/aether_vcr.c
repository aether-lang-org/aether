/* std.http.server.vcr — replay-driven HTTP server for testing.
 *
 * Tape format: Servirtium markdown (https://servirtium.dev). One
 * tape file holds N "interactions"; each interaction is a request +
 * response pair. The parser strategy is split-on-delimiter, the same
 * strategy other Servirtium implementations chose without it being
 * documented (the Java implementation's lock-step parsing is widely
 * regarded as a mistake — the markdown grammar splits cleanly).
 *
 * Three delimiters do all the work:
 *
 *   "\n## Interaction "   – between interactions
 *   "\n### "              – between sections within an interaction
 *   "\n```\n"             – fenced code block boundaries
 *
 * Within each section we look for the (single) fenced code block
 * and capture its contents verbatim. No state machine, no header
 * accumulator, no quote-counting.
 *
 * Section names in canonical Servirtium order:
 *
 *   ### Request headers recorded for playback:
 *   ### Request body recorded for playback (mime/type):
 *   ### Response headers recorded for playback:
 *   ### Response body recorded for playback (status: mime/type):
 *
 * Status code lives in the response-body section header (e.g.
 * "200: text/plain"), not in a status-line in the response-headers
 * block. v0.1 only reads the status from there; response headers
 * the tape lists are not echoed in the emitted response (they will
 * be in v0.2; the test matrix doesn't need them yet).
 *
 * v0.1 surface (matches std/http/server/vcr/module.ae's externs):
 *
 *   vcr_load_tape(path)        -> 1 on success, 0 on parse / I/O failure
 *   vcr_load_err()             -> error string (when vcr_load_tape returned 0)
 *   vcr_tape_length()          -> number of interactions the loaded tape has
 *   register_routes(srv)       -> Aether-side route registration walks the
 *                                  loaded tape and points matching routes at
 *                                  vcr_dispatch.
 *   vcr_dispatch(req, res, ud) -> registered handler. Matches the next tape
 *                                  interaction against (method, path) and
 *                                  emits the recorded response. Mismatch →
 *                                  599 with diagnostic body.
 *
 * Storage: a single global tape (one per process — adequate for v0.1
 * since each test runs as its own process). Loading a second tape
 * frees the first.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../string/aether_string.h"
#include "../../../../runtime/aether_resource_caps.h"

/* --- These come from std.http.server's request/response surface. --- */
extern const char* http_request_method(void* req);
extern const char* http_request_path(void* req);
extern void        http_response_set_status(void* res, int code);
extern void        http_response_set_header(void* res, const char* name, const char* value);
extern void        http_response_add_header(void* res, const char* name, const char* value);
extern void        http_response_clear_headers(void* res);
extern void        http_response_set_body  (void* res, const char* body);
extern void        http_response_set_body_n(void* res, const char* body, int length);

/* std.http.client v2 raw surface. Record mode uses these to forward
 * the incoming request to the real upstream service. This is an
 * ordinary client request, not an HTTP proxy tunnel. */
extern void*       http_request_raw(const char* method, const char* url);
extern int         http_request_set_header_raw(void* req, const char* name, const char* value);
extern int         http_request_set_body_raw(void* req, const char* body, int length, const char* content_type);
extern int         http_request_set_timeout_raw(void* req, int seconds);
extern void        http_request_free_raw(void* req);
extern void*       http_send_raw(void* req);
extern int         http_response_status(void* response);
extern const char* http_response_error(void* response);
extern void        http_response_free(void* response);

/* std.zlib gzip-framed helpers. VCR treats HTTP gzip as a transport
 * encoding: tapes store decoded semantic bodies, playback restores
 * gzip only when the caller advertises Accept-Encoding: gzip. */
extern int         zlib_backend_available(void);
extern int         zlib_try_gzip_inflate(const char* data, int length);
extern const char* zlib_get_inflate_bytes(void);
extern int         zlib_get_inflate_length(void);
extern void        zlib_release_inflate(void);
extern int         zlib_try_gzip_deflate(const char* data, int length, int level);
extern const char* zlib_get_deflate_bytes(void);
extern int         zlib_get_deflate_length(void);
extern void        zlib_release_deflate(void);

/* Step 10: header iteration + body access. We need to walk the
 * full set of request headers (not just look one up by name) to
 * detect "extra" headers the tape didn't expect. The HttpRequest
 * struct's prefix layout is stable across the v2 server — we
 * mirror just the fields we touch. Keep this in sync with
 * std/net/aether_http_server.h's HttpRequest definition. */
typedef struct VcrHttpRequestPrefix {
    char*  method;
    char*  path;
    char*  query_string;
    char*  http_version;
    char** header_keys;
    char** header_values;
    int    header_count;
    char*  body;
    size_t body_length;
    /* (rest of struct ignored — params, ...) */
} VcrHttpRequestPrefix;

typedef struct VcrAetherStringPrefix {
    unsigned int magic;
    int          ref_count;
    size_t       length;
    size_t       capacity;
    char*        data;
} VcrAetherStringPrefix;

typedef struct VcrClientResponsePrefix {
    int status_code;
    VcrAetherStringPrefix* body;
    VcrAetherStringPrefix* headers;
    VcrAetherStringPrefix* error;
    VcrAetherStringPrefix* redirect_error;
    VcrAetherStringPrefix* effective_url;
} VcrClientResponsePrefix;

/* Step 11: static-content serving externs. The http_server module
 * already implements the heavy lifting (mime-type detection,
 * realpath canonicalization, traversal-prevention) via
 * http_serve_file + http_serve_static. We reuse http_serve_file
 * directly so we can prepend the mount-prefix-stripping ourselves
 * and avoid double-prefix path construction. */
extern void http_serve_file(void* res, const char* filepath);
extern const char* http_mime_type(const char* path);

/* And the routing extern. Replay routes are enumerated in Aether;
 * record/static routes still need a C call because their handlers
 * live here. add_route takes the method as a string and so handles
 * any custom verb without per-verb dispatch in C. */
extern void http_server_add_route(void* server, const char* method, const char* path, void* handler, void* user_data);

/* ---- Tape storage --------------------------------------------------- */

typedef struct Interaction {
    char* method;       /* "GET" — owned, NUL-terminated */
    char* path;         /* "/path/to/resource" — owned */
    int   status;       /* 200, 404, ... */
    char* body;         /* response body — owned, NUL-terminated; "" if empty */
    char* content_type; /* may be NULL */
    /* Optional [Note] block (Servirtium step 9). Record-only — playback
     * ignores it. NULL when no note was attached to this interaction. */
    char* note_title;
    char* note_body;
    /* Step 10: request match fields. The parser fills these from the
     * `### Request headers ...` / `### Request body ...` code blocks.
     * Empty string ("") means the tape didn't constrain that field
     * — dispatcher skips comparison and matches on (method, path) only.
     * Non-empty → dispatcher must match the incoming request against
     * this exactly (after normalization), or fail with a diagnostic
     * the test reads via vcr.last_error(). */
    char* req_headers;  /* canonical-form: "Name: Value\n..." sorted by name */
    char* req_body;     /* request body bytes, "" if absent / empty */
    /* Response headers from the tape's `### Response headers recorded
     * for playback:` block. Parsed but not previously propagated —
     * dispatcher now walks this and calls http_response_set_header
     * per line, so tapes can declare arbitrary response headers
     * (X-* trace IDs, ETags, custom auth tokens) and have them
     * appear in the replayed response. Empty string = tape didn't
     * specify any response headers; dispatcher sets only Content-
     * Type from the response-body header line as before. */
    char* resp_headers;
} Interaction;

static Interaction* g_tape       = NULL;
static int          g_tape_n     = 0;
static int          g_tape_cap   = 0;
static int          g_tape_cursor = 0;
static char*        g_tape_err   = NULL;

/* Pending note staged via vcr_note() — attaches to the next
 * vcr_record_interaction() call, then clears. NULL when nothing
 * is staged. */
static char* g_pending_note_title = NULL;
static char* g_pending_note_body  = NULL;

/* Step 10: last playback mismatch diagnostic. The dispatcher
 * populates this when an incoming request doesn't match the
 * next-in-line tape entry (extra header, different body, etc).
 * The test's tearDown reads it via vcr.last_error(). NULL or ""
 * means "no error since last clear". */
static char* g_last_error = NULL;

/* Per-dispatch outcome slots — siblings of g_last_error. Tests check
 * these after each request. Single slot per axis (kind / index) on
 * the assumption tests drive VCR serially and inspect immediately.
 * If the test misses a slot read, the next dispatch overwrites; the
 * test is responsible for the cadence, not VCR. Models the Java
 * Servirtium ServiceMonitor's per-interaction notification but as
 * a passive read surface (no callback, no allocation, no actor). */
#define VCR_KIND_OK                  0
#define VCR_KIND_PATH_OR_METHOD_DIFF 1
#define VCR_KIND_HEADER_MISSING      2
#define VCR_KIND_HEADER_VALUE_DIFF   3
#define VCR_KIND_HEADER_UNEXPECTED   4
#define VCR_KIND_TAPE_EXHAUSTED      5
#define VCR_KIND_BODY_DIFF           6
#define VCR_KIND_RECORD_ERROR        7
#define VCR_FIELD_PATH             1
#define VCR_FIELD_RESPONSE_BODY    2
#define VCR_FIELD_REQUEST_HEADERS  3
#define VCR_FIELD_REQUEST_BODY     4
#define VCR_FIELD_RESPONSE_HEADERS 5
static int g_last_kind = VCR_KIND_OK;
static int g_last_index = -1;

/* Strict-match opt-in. When 0 (default), the dispatcher matches on
 * (method, path) only and doesn't check request headers / body
 * unless the tape itself constrains them (the implicit gate at
 * line ~786). When 1, request-header and request-body checks fire
 * unconditionally — used by tests that want to deliberately
 * exercise the mismatch diagnostic paths. */
static int g_strict_headers = 0;
static char* g_record_upstream_base = NULL;

static void tape_free_storage(void) {
    if (g_tape) {
        for (int i = 0; i < g_tape_n; i++) {
            free(g_tape[i].method);
            free(g_tape[i].path);
            free(g_tape[i].body);
            free(g_tape[i].content_type);
            free(g_tape[i].note_title);
            free(g_tape[i].note_body);
            free(g_tape[i].req_headers);
            free(g_tape[i].req_body);
            free(g_tape[i].resp_headers);
        }
        free(g_tape);
        g_tape = NULL;
    }
    g_tape_n = 0;
    g_tape_cap = 0;
    g_tape_cursor = 0;
    free(g_tape_err);
    g_tape_err = NULL;
    free(g_pending_note_title); g_pending_note_title = NULL;
    free(g_pending_note_body);  g_pending_note_body  = NULL;
    free(g_last_error);         g_last_error         = NULL;
    free(g_record_upstream_base); g_record_upstream_base = NULL;
    g_last_kind = VCR_KIND_OK;
    g_last_index = -1;
    /* Static mounts intentionally not freed here — mounts are
     * configured by the caller before vcr.load() and survive across
     * loads in the same process; vcr.clear_static_content() is the
     * explicit reset. */
}

static void last_err_set(const char* msg) {
    free(g_last_error);
    g_last_error = msg ? strdup(msg) : NULL;
}

static void record_dispatch_error(void* res, int status, const char* msg) {
    last_err_set(msg);
    g_last_kind = VCR_KIND_RECORD_ERROR;
    g_last_index = -1;
    http_response_set_status(res, status);
    http_response_set_body(res, msg);
}

static void tape_set_err(const char* msg) {
    free(g_tape_err);
    g_tape_err = msg ? strdup(msg) : NULL;
}

static int ascii_ci_contains(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) return 0;
    size_t nlen = strlen(needle);
    for (const char* h = haystack; *h; h++) {
        size_t i = 0;
        while (i < nlen && h[i]) {
            char a = h[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            i++;
        }
        if (i == nlen) return 1;
    }
    return 0;
}

static int icmp(const char* a, const char* b);

static int b64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int)(c - 'a') + 26;
    if (c >= '0' && c <= '9') return (int)(c - '0') + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -2;
    return -1;
}

static int b64_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int decode_base64_body(const char* src, unsigned char** out, int* out_len) {
    if (!src || !out || !out_len) return 0;
    *out = NULL;
    *out_len = 0;

    /* Cap-aware (#343): src is tape content (untrusted in plugin-host
     * scenarios). Both the cleanup buffer and the decoded-output
     * buffer are gated; out-buffer is handed back to the caller who
     * releases via plain libc free (caller-owned-return drift). */
    size_t src_len = strlen(src);
    size_t clean_cap = src_len + 4;
    unsigned char* clean = (unsigned char*)aether_caps_malloc(clean_cap);
    if (!clean) return 0;

    size_t clen = 0;
    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (b64_is_space(c)) continue;
        clean[clen++] = c;
    }
    while (clen % 4 != 0) clean[clen++] = '=';

    size_t buf_cap = (clen / 4) * 3 + 1;
    unsigned char* buf = (unsigned char*)aether_caps_malloc(buf_cap);
    if (!buf) {
        aether_caps_free(clean, clean_cap);
        return 0;
    }

    size_t off = 0;
    for (size_t i = 0; i < clen; i += 4) {
        int v0 = b64_value(clean[i]);
        int v1 = b64_value(clean[i + 1]);
        int v2 = b64_value(clean[i + 2]);
        int v3 = b64_value(clean[i + 3]);
        if (v0 < 0 || v1 < 0 || v2 == -1 || v3 == -1) {
            aether_caps_free(clean, clean_cap);
            aether_caps_free(buf, buf_cap);
            return 0;
        }
        if (v2 == -2 && v3 != -2) {
            aether_caps_free(clean, clean_cap);
            aether_caps_free(buf, buf_cap);
            return 0;
        }

        buf[off++] = (unsigned char)((v0 << 2) | (v1 >> 4));
        if (v2 != -2) {
            buf[off++] = (unsigned char)(((v1 & 0x0f) << 4) | (v2 >> 2));
        }
        if (v3 != -2) {
            buf[off++] = (unsigned char)(((v2 & 0x03) << 6) | v3);
        }
    }

    aether_caps_free(clean, clean_cap);
    buf[off] = '\0';
    *out = buf;
    *out_len = (int)off;
    return 1;
}

AetherString* vcr_decode_base64_body_raw(const char* src) {
    unsigned char* decoded = NULL;
    int decoded_len = 0;
    if (!decode_base64_body(src, &decoded, &decoded_len)) return NULL;
    AetherString* result = string_new_with_length((const char*)decoded, (size_t)decoded_len);
    /* decoded was allocated by decode_base64_body via aether_caps_malloc
     * but its capacity isn't threaded back here; the libc-compatible-
     * pointer contract (aether_resource_caps.h:89-94) keeps the free
     * heap-safe at the cost of cap-counter drift on this path. */
    free(decoded);
    return result;
}

static int tape_append(const char* method, const char* path,
                       int status, const char* body, const char* content_type,
                       const char* req_headers, const char* req_body) {
    if (g_tape_n >= g_tape_cap) {
        int new_cap = g_tape_cap ? g_tape_cap * 2 : 8;
        Interaction* bigger = (Interaction*)realloc(g_tape, sizeof(Interaction) * (size_t)new_cap);
        if (!bigger) return -1;
        g_tape = bigger;
        g_tape_cap = new_cap;
    }
    Interaction* e = &g_tape[g_tape_n];
    e->method       = strdup(method);
    e->path         = strdup(path);
    e->status       = status;
    e->body         = strdup(body ? body : "");
    e->content_type = content_type ? strdup(content_type) : NULL;
    e->req_headers  = strdup(req_headers ? req_headers : "");
    e->req_body     = strdup(req_body ? req_body : "");
    /* resp_headers starts empty — the Aether-side parser fills it
     * later by calling vcr_aether_set_resp_headers(idx, value) when
     * a `### Response headers recorded for playback:` block is
     * present in the tape. Empty string means dispatcher emits only
     * Content-Type (the legacy single-header path). */
    e->resp_headers = strdup("");
    /* Drain the pending-note slot — ownership transfers to this
     * interaction, so the note attaches to exactly one capture. */
    e->note_title   = g_pending_note_title;
    e->note_body    = g_pending_note_body;
    g_pending_note_title = NULL;
    g_pending_note_body  = NULL;
    if (!e->method || !e->path || !e->body || (content_type && !e->content_type)
        || !e->req_headers || !e->req_body || !e->resp_headers) {
        free(e->method); free(e->path); free(e->body); free(e->content_type);
        free(e->req_headers); free(e->req_body); free(e->resp_headers);
        free(e->note_title); free(e->note_body);
        return -1;
    }
    g_tape_n++;
    return 0;
}

/* ---- Helpers --------------------------------------------------------- */

/* slurp / slice_dup / rtrim deleted in the C→Aether parser port —
 * the Aether parser uses fs.read + string.substring + an Aether
 * rtrim() helper, none of which need C. See module.ae's
 * parse_tape_file. */

/* Forward decl for the dispatcher — referenced by Aether route
 * registration and by the static-mount route registration. Body lives further
 * down (after the request-normalization helpers). */
void vcr_dispatch(void* req, void* res, void* ud);
static void vcr_record_dispatch(void* req, void* res, void* ud);
int vcr_record_interaction_full(const char* method, const char* path,
                                int status, const char* content_type, const char* body,
                                const char* req_headers, const char* req_body);
void vcr_aether_set_resp_headers(int index, const char* headers);

const char* vcr_load_err(void) {
    return g_tape_err ? g_tape_err : "";
}

int vcr_tape_length(void) {
    return g_tape_n;
}

/* Forward decl — body lives next to the static-mount storage. */
static void register_static_routes(void* server);
static void strip_trailing_slash(char* s);
static char* build_recorded_path(const VcrHttpRequestPrefix* r);

void vcr_register_static_routes(void* server) {
    register_static_routes(server);
}

int vcr_register_record_routes(void* server, const char* upstream_base) {
    if (!server || !upstream_base || !*upstream_base) {
        tape_set_err("vcr.record mode: null server or upstream_base");
        return 0;
    }
    char* copy = strdup(upstream_base);
    if (!copy) {
        tape_set_err("OOM storing record-mode upstream base");
        return 0;
    }
    strip_trailing_slash(copy);
    free(g_record_upstream_base);
    g_record_upstream_base = copy;

    http_server_add_route(server, "*", "*", (void*)vcr_record_dispatch, NULL);
    register_static_routes(server);
    return 1;
}

/* ---- Record-mode externs ---------------------------------------------
 *
 * Record mode is driven from the Aether side. The recorder's HTTP
 * handler calls std.http.client to forward the request to upstream,
 * then calls vcr_record_interaction() with the method/path/status/
 * content_type/body it just observed. On vcr.eject(), the Aether
 * side calls vcr_flush_to_tape(path) which writes every captured
 * interaction to disk in canonical Servirtium markdown — the same
 * format the parser above reads, so a recorded tape is immediately
 * replayable on the next test run.
 *
 * The capture list reuses g_tape as its backing storage. A test
 * that loads a tape and then records into the same VCR instance
 * would mix them — fine for v0.1 since record/replay are mutually
 * exclusive per-run.
 * -------------------------------------------------------------------- */

int vcr_record_interaction_full(const char* method, const char* path,
                                int status, const char* content_type, const char* body,
                                const char* req_headers, const char* req_body);

int vcr_record_interaction(const char* method, const char* path,
                           int status, const char* content_type, const char* body) {
    /* Step-7-era recorder: only response bytes, no request capture.
     * Forwards to the step-10 entry point with empty req_headers /
     * req_body — empty means "no constraint" so on later replay the
     * dispatcher matches on (method, path) only, identical to the
     * pre-step-10 behavior. */
    return vcr_record_interaction_full(method, path, status, content_type, body, "", "");
}

/* Step 10 record entry point: like vcr_record_interaction but also
 * captures the request headers (canonical-form: "Name: Value\n..."
 * sorted by name, Host stripped) and request body. Pass "" for
 * either to opt that field out of mismatch detection. */
int vcr_record_interaction_full(const char* method, const char* path,
                                int status, const char* content_type, const char* body,
                                const char* req_headers, const char* req_body) {
    if (!method || !path) { tape_set_err("null method or path"); return 0; }
    if (tape_append(method, path, status, body, content_type,
                    req_headers ? req_headers : "",
                    req_body    ? req_body    : "") != 0) {
        tape_set_err("OOM appending captured interaction");
        return 0;
    }
    return 1;
}

/* Step 9: Notes — record-only annotations the test author can attach
 * to a specific interaction. Stages a (title, body) pair; the next
 * vcr_record_interaction call drains the stage onto the new
 * interaction. Calling vcr_add_note() twice in a row without an
 * intervening record() replaces the staged note (last writer wins).
 *
 * Playback ignores notes — they're parser-tolerated but never
 * surfaced. Notes survive across flush() but not across
 * vcr_clear_tape() / load(). */
int vcr_add_note(const char* title, const char* body) {
    if (!title) { tape_set_err("vcr_add_note: null title"); return 0; }
    char* t = strdup(title);
    char* b = strdup(body ? body : "");
    if (!t || !b) {
        free(t); free(b);
        tape_set_err("OOM staging note");
        return 0;
    }
    free(g_pending_note_title);
    free(g_pending_note_body);
    g_pending_note_title = t;
    g_pending_note_body  = b;
    return 1;
}

/* ---- Step 8: redactions (Servirtium "mutation operations") -----------
 *
 * Pattern → replacement substitutions applied at flush time, against
 * specific fields of each interaction. The in-memory tape stays
 * pristine (so the SUT sees real responses during recording);
 * redactions only affect what gets written to disk and what gets
 * compared in flush_or_check.
 *
 * v0.1: substring match only (no regex). Field selectors limited to
 * what the recorder actually captures today — `path` and
 * `response_body`. Adding `request_headers` etc. is straightforward
 * once those fields land in the recorder.
 *
 * Use case: scrub Authorization tokens, session cookies, API keys
 * embedded in URLs, server-issued ids that would otherwise leak
 * into a public repo. Test author calls vcr.redact(field, pattern,
 * replacement) once per pattern before flush; the flush walks each
 * interaction and applies every registered redaction.
 * -------------------------------------------------------------------- */

typedef struct Redaction {
    int   field;        /* VCR_FIELD_* */
    char* pattern;      /* substring to match, owned */
    char* replacement;  /* what to put in its place, owned */
} Redaction;

typedef struct HeaderRemoval {
    int   field;        /* VCR_FIELD_REQUEST_HEADERS / RESPONSE_HEADERS */
    char* name;         /* header name, owned */
} HeaderRemoval;

static Redaction* g_redactions     = NULL;
static int        g_redactions_n   = 0;
static int        g_redactions_cap = 0;
static Redaction* g_unredactions     = NULL;
static int        g_unredactions_n   = 0;
static int        g_unredactions_cap = 0;
static HeaderRemoval* g_header_removals     = NULL;
static int            g_header_removals_n   = 0;
static int            g_header_removals_cap = 0;

static void redaction_list_free(Redaction** items, int* n_items, int* cap_items) {
    if (*items) {
        for (int i = 0; i < *n_items; i++) {
            free((*items)[i].pattern);
            free((*items)[i].replacement);
        }
        free(*items);
        *items = NULL;
    }
    *n_items = 0;
    *cap_items = 0;
}

static int valid_redaction_field(int field) {
    return field == VCR_FIELD_PATH
        || field == VCR_FIELD_RESPONSE_BODY
        || field == VCR_FIELD_REQUEST_HEADERS
        || field == VCR_FIELD_REQUEST_BODY
        || field == VCR_FIELD_RESPONSE_HEADERS;
}

static int redaction_list_add(Redaction** items, int* n_items, int* cap_items,
                              int field, const char* pattern, const char* replacement,
                              const char* api_name) {
    if (!valid_redaction_field(field)) {
        tape_set_err("unsupported field selector");
        return 0;
    }
    if (!pattern) { tape_set_err("null pattern"); return 0; }
    if (*n_items >= *cap_items) {
        int new_cap = *cap_items ? *cap_items * 2 : 4;
        Redaction* bigger = (Redaction*)realloc(*items, sizeof(Redaction) * (size_t)new_cap);
        if (!bigger) {
            char msg[128];
            snprintf(msg, sizeof(msg), "OOM growing %s list", api_name);
            tape_set_err(msg);
            return 0;
        }
        *items = bigger;
        *cap_items = new_cap;
    }
    Redaction* r = &(*items)[*n_items];
    r->field = field;
    r->pattern = strdup(pattern);
    r->replacement = strdup(replacement ? replacement : "");
    if (!r->pattern || !r->replacement) {
        free(r->pattern); free(r->replacement);
        tape_set_err("OOM appending replacement");
        return 0;
    }
    *n_items = *n_items + 1;
    return 1;
}

int vcr_add_redaction(int field, const char* pattern, const char* replacement) {
    return redaction_list_add(&g_redactions, &g_redactions_n, &g_redactions_cap,
                              field, pattern, replacement, "redaction");
}

int vcr_add_unredaction(int field, const char* pattern, const char* replacement) {
    return redaction_list_add(&g_unredactions, &g_unredactions_n, &g_unredactions_cap,
                              field, pattern, replacement, "unredaction");
}

static int valid_header_removal_field(int field) {
    return field == VCR_FIELD_REQUEST_HEADERS || field == VCR_FIELD_RESPONSE_HEADERS;
}

int vcr_add_header_removal(int field, const char* name) {
    if (!valid_header_removal_field(field)) {
        tape_set_err("header removal only supports request/response header fields");
        return 0;
    }
    if (!name || !*name) {
        tape_set_err("header removal requires a header name");
        return 0;
    }
    if (g_header_removals_n >= g_header_removals_cap) {
        int new_cap = g_header_removals_cap ? g_header_removals_cap * 2 : 4;
        HeaderRemoval* bigger = (HeaderRemoval*)realloc(g_header_removals,
                                      sizeof(HeaderRemoval) * (size_t)new_cap);
        if (!bigger) {
            tape_set_err("OOM growing header removal list");
            return 0;
        }
        g_header_removals = bigger;
        g_header_removals_cap = new_cap;
    }
    HeaderRemoval* h = &g_header_removals[g_header_removals_n];
    h->field = field;
    h->name = strdup(name);
    if (!h->name) {
        tape_set_err("OOM appending header removal");
        return 0;
    }
    g_header_removals_n++;
    return 1;
}

static int header_removed_for_field(int field, const char* name) {
    if (!name) return 0;
    for (int i = 0; i < g_header_removals_n; i++) {
        if (g_header_removals[i].field == field && icmp(g_header_removals[i].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void header_removals_free_storage(void) {
    if (g_header_removals) {
        for (int i = 0; i < g_header_removals_n; i++) {
            free(g_header_removals[i].name);
        }
        free(g_header_removals);
        g_header_removals = NULL;
    }
    g_header_removals_n = 0;
    g_header_removals_cap = 0;
}

/* Step 12: optional markdown formatting toggles for the recorder.
 * Read by the Aether-side emitter via vcr_get_indent_code_blocks /
 * vcr_get_emphasize_http_verbs. Defaults off so existing tapes
 * round-trip byte-identical when no toggle is set. */
static int g_indent_code_blocks  = 0;
static int g_emphasize_http_verbs = 0;

/* Emitter ported to Aether — see std/http/server/vcr/module.ae's
 * emit_tape() / emit_one_interaction() / emit_code_block(). The
 * Aether emitter walks the C-side tape via vcr_get_* accessors,
 * builds the markdown blob, and writes it via fs.write. The C
 * emitter that lived here previously was deleted in the same
 * commit that introduced the Aether one. */

/* Drop all registered redactions. Useful to call between tests in
 * the same process so a redaction set doesn't leak across them. */
void vcr_clear_redactions(void) {
    redaction_list_free(&g_redactions, &g_redactions_n, &g_redactions_cap);
}

void vcr_clear_unredactions(void) {
    redaction_list_free(&g_unredactions, &g_unredactions_n, &g_unredactions_cap);
}

void vcr_clear_header_removals(void) {
    header_removals_free_storage();
}

/* Step 12 setters — opt the recorder into the alternative markdown
 * forms. Default off → fenced blocks + bare verbs (current
 * behavior, byte-identical re-flush of existing tapes). Playback
 * tolerates either form regardless of these toggles. */
void vcr_set_indent_code_blocks(int on)   { g_indent_code_blocks  = on ? 1 : 0; }
void vcr_set_emphasize_http_verbs(int on) { g_emphasize_http_verbs = on ? 1 : 0; }

/* ---- Step 11: static-content serving --------------------------------
 *
 * Mark an on-disk directory as bypass-the-tape territory. Every
 * incoming GET whose path starts with `mount_path` is served from
 * disk (with the existing http_serve_file's mime-type detection
 * and traversal-prevention) and never consulted against the tape.
 *
 * Use case: Selenium/Cypress/Playwright UI tests where the page
 * is the SUT — letting the tape capture every CSS/JS/image asset
 * is noise. Point the static-content config at the local build
 * artifacts directory; the tape stays focused on the actual
 * domain-API exchanges the test cares about.
 *
 * Lookup model: dispatcher's wildcard route `<mount>` followed by
 * /-star is bound
 * to vcr_static_dispatch with a heap StaticMount* as user_data.
 * The handler strips the mount prefix from req->path, joins
 * against fs_dir, and forwards to http_serve_file (whose realpath
 * + mime-type machinery we get for free).
 *
 * v0.1: GET only (matches the spec). Multiple non-overlapping
 * mounts are fine. Overlapping mounts (e.g. /a and /a/b) work in
 * route-registration order — first registered wins.
 * -------------------------------------------------------------------- */

typedef struct StaticMount {
    char* mount_path;    /* request prefix, e.g. "/static" — owned, no trailing slash */
    char* fs_dir;        /* on-disk dir, e.g. "/tmp/static_root" — owned, no trailing slash */
} StaticMount;

static StaticMount* g_mounts     = NULL;
static int          g_mounts_n   = 0;
static int          g_mounts_cap = 0;

static void mounts_free_storage(void) {
    if (g_mounts) {
        for (int i = 0; i < g_mounts_n; i++) {
            free(g_mounts[i].mount_path);
            free(g_mounts[i].fs_dir);
        }
        free(g_mounts);
        g_mounts = NULL;
    }
    g_mounts_n = 0;
    g_mounts_cap = 0;
}

/* Strip a single trailing '/' (other than a lone-root path) so
 * concatenation doesn't double-slash. mutates `s` in place. */
static void strip_trailing_slash(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    if (n > 1 && s[n - 1] == '/') s[n - 1] = '\0';
}

/* Dispatcher for a static-mount route. user_data is the
 * StaticMount* registered alongside it. */
static void vcr_static_dispatch(void* req, void* res, void* ud) {
    StaticMount* m = (StaticMount*)ud;
    if (!m) {
        http_response_set_status(res, 500);
        http_response_set_body(res, "static dispatch: null mount");
        return;
    }
    const char* path = http_request_path(req);
    if (!path) {
        http_response_set_status(res, 400);
        http_response_set_body(res, "static dispatch: null request path");
        return;
    }

    /* Strip the mount prefix. Both `mount_path` and `path` start
     * with '/'; after strlen(mount_path) chars, what remains is
     * the asset's relative path under fs_dir (with a leading
     * '/' if the request had it). */
    size_t mlen = strlen(m->mount_path);
    if (strncmp(path, m->mount_path, mlen) != 0) {
        http_response_set_status(res, 500);
        http_response_set_body(res, "static dispatch: prefix mismatch (router bug)");
        return;
    }
    const char* rel = path + mlen;
    if (*rel == '/') rel++;
    if (*rel == '\0') rel = "index.html";

    /* Belt-and-braces traversal guard. http_serve_file's
     * realpath check below also catches escapes, but the
     * percent-encoded variants need this string-level rejection
     * up front. Mirrors http_serve_static's checks. */
    if (strstr(rel, "..") != NULL
        || strstr(rel, "%2e") != NULL || strstr(rel, "%2E") != NULL
        || strstr(rel, "%2f") != NULL || strstr(rel, "%2F") != NULL
        || strstr(rel, "%5c") != NULL || strstr(rel, "%5C") != NULL
        || strstr(rel, "\\") != NULL) {
        http_response_set_status(res, 403);
        http_response_set_body(res, "403 - Forbidden");
        return;
    }

    char filepath[1024];
    int n = snprintf(filepath, sizeof(filepath), "%s/%s", m->fs_dir, rel);
    if (n < 0 || (size_t)n >= sizeof(filepath)) {
        http_response_set_status(res, 414);
        http_response_set_body(res, "414 - URI Too Long");
        return;
    }
    http_serve_file(res, filepath);
}

/* Register a static-content mount. Append to g_mounts; the
 * actual route registration happens in Aether register_routes
 * (which is called by vcr.load — load order is: register tape
 * routes, then mounts; routes overlap is fine since
 * mount-rooted requests don't appear in tapes by definition). */
int vcr_add_static_content(const char* mount_path, const char* fs_dir) {
    if (!mount_path || !fs_dir) {
        tape_set_err("vcr_add_static_content: null mount_path or fs_dir");
        return 0;
    }
    if (mount_path[0] != '/') {
        tape_set_err("vcr_add_static_content: mount_path must start with '/'");
        return 0;
    }
    if (g_mounts_n >= g_mounts_cap) {
        int new_cap = g_mounts_cap ? g_mounts_cap * 2 : 4;
        StaticMount* bigger = (StaticMount*)realloc(g_mounts,
                                  sizeof(StaticMount) * (size_t)new_cap);
        if (!bigger) { tape_set_err("OOM growing mounts"); return 0; }
        g_mounts = bigger;
        g_mounts_cap = new_cap;
    }
    StaticMount* m = &g_mounts[g_mounts_n];
    m->mount_path = strdup(mount_path);
    m->fs_dir     = strdup(fs_dir);
    if (!m->mount_path || !m->fs_dir) {
        free(m->mount_path); free(m->fs_dir);
        tape_set_err("OOM appending mount");
        return 0;
    }
    strip_trailing_slash(m->mount_path);
    strip_trailing_slash(m->fs_dir);
    g_mounts_n++;
    return 1;
}

/* Drop all static-content mounts. The route registrations against
 * the http server stay live until the server is torn down — calling
 * this between tests prevents stale mount config bleeding into
 * the next test, but the recommended pattern is one VCR per test. */
void vcr_clear_static_content(void) {
    mounts_free_storage();
}

/* Register the wildcard routes for every mount with the http
 * server. Called from Aether register_routes() after the tape
 * routes go in. */
static void register_static_routes(void* server) {
    if (!server) return;
    for (int i = 0; i < g_mounts_n; i++) {
        char pattern[1024];
        int n = snprintf(pattern, sizeof(pattern), "%s/*", g_mounts[i].mount_path);
        if (n < 0 || (size_t)n >= sizeof(pattern)) continue;
        http_server_add_route(server, "GET", pattern,
                              (void*)vcr_static_dispatch, &g_mounts[i]);
    }
}

/* vcr_flush_to_tape and vcr_flush_or_check_raw deleted in the
 * C→Aether emitter port. Their replacements live in module.ae as
 * vcr.flush() and vcr.flush_or_check(), built on top of the Aether
 * emit_tape() and fs.read / fs.write. The dispatcher and the
 * tape-storage globals stayed in C since the http_server hands us
 * function pointers, but everything that's pure string assembly
 * is Aether now. */

/* ---- Step 10: request normalization + match ----
 *
 * Goal: detect when an incoming request differs from what the
 * tape was recorded against (extra header, missing header, wrong
 * value, different body), and fail loudly with a diagnostic the
 * test reads via vcr.last_error().
 *
 * Both sides — the recorded blob in `e->req_headers` and the
 * incoming request's headers — are reduced to a common canonical
 * form before comparison: each header rendered as "Name: Value"
 * on its own line, sorted ascending by lowercased name, with
 * Host stripped (Host is always test-host-controlled and would
 * cause false positives on every record-vs-replay diff).
 *
 * The recording side is responsible for emitting the canonical
 * form into the tape; the dispatcher only normalizes the live
 * incoming side. Tapes hand-edited by humans aren't guaranteed
 * canonical — the spec's broken_recordings/ examples have one
 * header so order doesn't matter, but adding multi-header tests
 * would need the loaded blob normalized too. v0.1 leaves that
 * for future work; today's hand-edited tapes are short enough
 * to keep canonical by inspection.
 */

static int icmp(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* qsort comparator over an array of "Name: Value" lines (no
 * trailing newline). Sort key is the name (everything before
 * the first ':'), case-insensitive. */
static int line_cmp(const void* lhs, const void* rhs) {
    const char* a = *(const char* const*)lhs;
    const char* b = *(const char* const*)rhs;
    return icmp(a, b);
}


/* Build the canonical "Name: Value\n..." blob from an
 * HttpRequest's parallel keys/values arrays. Drops Host.
 * Returns malloc'd string the caller frees, or NULL on OOM. */
static char* normalize_live_headers(const VcrHttpRequestPrefix* r) {
    if (r->header_count <= 0) return strdup("");

    /* Collect non-Host lines into a heap array. */
    char** lines = (char**)calloc((size_t)r->header_count, sizeof(char*));
    if (!lines) return NULL;
    int n_lines = 0;
    for (int i = 0; i < r->header_count; i++) {
        const char* key = r->header_keys[i];
        const char* val = r->header_values[i] ? r->header_values[i] : "";
        if (!key) continue;
        /* Drop wire-layer headers that the recorder shouldn't have
         * captured and the replay can't faithfully reproduce. Host
         * is computed from the URL by std.http.client; Connection
         * and Content-Length are HTTP/1.1 transport concerns
         * (keep-alive state, body length) that aren't part of the
         * recorded protocol. Same exclusion list as the response-
         * side normalizer in test/probe code. */
        if (icmp(key, "Host") == 0) continue;
        if (icmp(key, "Connection") == 0) continue;
        if (icmp(key, "Content-Length") == 0) continue;
        if (icmp(key, "Accept-Encoding") == 0) continue;
        if (header_removed_for_field(VCR_FIELD_REQUEST_HEADERS, key)) continue;
        size_t len = strlen(key) + 2 + strlen(val) + 1;
        char* line = (char*)malloc(len);
        if (!line) {
            for (int j = 0; j < n_lines; j++) free(lines[j]);
            free(lines);
            return NULL;
        }
        snprintf(line, len, "%s: %s", key, val);
        lines[n_lines++] = line;
    }

    qsort(lines, (size_t)n_lines, sizeof(char*), line_cmp);

    /* Concatenate. */
    size_t total = 0;
    for (int i = 0; i < n_lines; i++) total += strlen(lines[i]) + 1; /* line + \n */
    char* out = (char*)malloc(total + 1);
    if (!out) {
        for (int i = 0; i < n_lines; i++) free(lines[i]);
        free(lines);
        return NULL;
    }
    char* dst = out;
    for (int i = 0; i < n_lines; i++) {
        size_t l = strlen(lines[i]);
        memcpy(dst, lines[i], l);
        dst[l] = '\n';
        dst += l + 1;
        free(lines[i]);
    }
    *dst = '\0';
    free(lines);
    return out;
}

/* True if `s` is empty or contains only whitespace. Used to
 * decide whether a captured field constrains matching at all
 * — the spec encodes "no constraint" as an empty code block. */
static int is_blank(const char* s) {
    if (!s) return 1;
    while (*s) {
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') return 0;
        s++;
    }
    return 1;
}

/* Pull the first header name out of `recorded` ("Name: ...\n..."),
 * up to MAX-1 chars, NUL-terminate. Used in diagnostics so we
 * can name a specific offending header in the error string. */
static void first_header_name(const char* recorded, char* out, size_t max) {
    size_t i = 0;
    while (recorded[i] && recorded[i] != ':' && recorded[i] != '\n' && i + 1 < max) {
        out[i] = recorded[i];
        i++;
    }
    out[i] = '\0';
}

static char* replace_all_c(const char* src, const char* pattern, const char* replacement) {
    if (!src) src = "";
    if (!pattern || !*pattern) return strdup(src);
    if (!replacement) replacement = "";

    size_t src_len = strlen(src);
    size_t pat_len = strlen(pattern);
    size_t rep_len = strlen(replacement);
    size_t count = 0;
    const char* p = src;
    while ((p = strstr(p, pattern)) != NULL) {
        count++;
        p += pat_len;
    }
    size_t out_len = src_len + count * (rep_len > pat_len ? rep_len - pat_len : 0);
    if (rep_len < pat_len) out_len = src_len - count * (pat_len - rep_len);
    char* out = (char*)malloc(out_len + 1);
    if (!out) return NULL;

    const char* cursor = src;
    char* dst = out;
    while ((p = strstr(cursor, pattern)) != NULL) {
        size_t chunk = (size_t)(p - cursor);
        memcpy(dst, cursor, chunk);
        dst += chunk;
        memcpy(dst, replacement, rep_len);
        dst += rep_len;
        cursor = p + pat_len;
    }
    strcpy(dst, cursor);
    return out;
}

static char* apply_unredactions_for_c(const char* src, int field) {
    char* out = strdup(src ? src : "");
    if (!out) return NULL;
    for (int i = 0; i < g_unredactions_n; i++) {
        if (g_unredactions[i].field != field) continue;
        char* next = replace_all_c(out, g_unredactions[i].pattern, g_unredactions[i].replacement);
        free(out);
        if (!next) return NULL;
        out = next;
    }
    return out;
}

static char* remove_headers_from_block_c(const char* headers, int field) {
    if (!headers) headers = "";
    char* out = (char*)malloc(strlen(headers) + 1);
    if (!out) return NULL;
    size_t off = 0;
    const char* p = headers;
    while (*p) {
        const char* line_start = p;
        const char* nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p) : strlen(p);
        size_t trimmed_len = line_len;
        if (trimmed_len > 0 && line_start[trimmed_len - 1] == '\r') trimmed_len--;
        int keep = 1;
        const char* colon = memchr(line_start, ':', trimmed_len);
        if (colon) {
            size_t name_len = (size_t)(colon - line_start);
            char name[256];
            if (name_len > 0 && name_len < sizeof(name)) {
                memcpy(name, line_start, name_len);
                name[name_len] = '\0';
                if (header_removed_for_field(field, name)) keep = 0;
            }
        }
        if (keep && line_len > 0) {
            memcpy(out + off, line_start, line_len);
            off += line_len;
            if (nl) out[off++] = '\n';
        }
        if (!nl) break;
        p = nl + 1;
    }
    out[off] = '\0';
    return out;
}

static const char* request_header_value(const VcrHttpRequestPrefix* r, const char* name) {
    if (!r || !name) return "";
    for (int i = 0; i < r->header_count; i++) {
        if (r->header_keys[i] && icmp(r->header_keys[i], name) == 0) {
            return r->header_values[i] ? r->header_values[i] : "";
        }
    }
    return "";
}

static int is_hop_by_hop_header(const char* name) {
    if (!name) return 1;
    return icmp(name, "Host") == 0
        || icmp(name, "Connection") == 0
        || icmp(name, "Content-Length") == 0
        || icmp(name, "Transfer-Encoding") == 0
        || icmp(name, "Keep-Alive") == 0
        || icmp(name, "Proxy-Authenticate") == 0
        || icmp(name, "Proxy-Authorization") == 0
        || icmp(name, "TE") == 0
        || icmp(name, "Trailer") == 0
        || icmp(name, "Upgrade") == 0;
}

static char* build_upstream_url(const char* base, const VcrHttpRequestPrefix* r) {
    const char* path = (r && r->path && r->path[0]) ? r->path : "/";
    const char* query = (r && r->query_string) ? r->query_string : "";
    size_t base_len = strlen(base ? base : "");
    size_t path_len = strlen(path);
    size_t query_len = strlen(query);
    int need_slash = (base_len > 0 && path[0] != '/');
    size_t total = base_len + (need_slash ? 1 : 0) + path_len + query_len;
    char* out = (char*)malloc(total + 1);
    if (!out) return NULL;
    char* p = out;
    if (base_len) { memcpy(p, base, base_len); p += base_len; }
    if (need_slash) *p++ = '/';
    memcpy(p, path, path_len); p += path_len;
    if (query_len) { memcpy(p, query, query_len); p += query_len; }
    *p = '\0';
    return out;
}

static char* build_recorded_path(const VcrHttpRequestPrefix* r) {
    const char* path = (r && r->path && r->path[0]) ? r->path : "/";
    const char* query = (r && r->query_string) ? r->query_string : "";
    size_t path_len = strlen(path);
    size_t query_len = strlen(query);
    char* out = (char*)malloc(path_len + query_len + 1);
    if (!out) return NULL;
    memcpy(out, path, path_len);
    if (query_len) memcpy(out + path_len, query, query_len);
    out[path_len + query_len] = '\0';
    return out;
}

static int expected_path_includes_query(const char* path) {
    return path && strchr(path, '?') != NULL;
}

static char* response_header_value_from_block(const char* headers, const char* name);

static char* response_headers_for_tape(const char* raw_headers, int strip_content_encoding) {
    if (!raw_headers) return strdup("");
    const char* p = strchr(raw_headers, '\n');
    p = p ? p + 1 : raw_headers;

    size_t cap = strlen(p) + 1;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    size_t off = 0;

    while (*p) {
        const char* line_start = p;
        const char* nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p) : strlen(p);
        if (line_len > 0 && line_start[line_len - 1] == '\r') line_len--;

        const char* colon = memchr(line_start, ':', line_len);
        if (colon) {
            size_t name_len = (size_t)(colon - line_start);
            char name[256];
            if (name_len > 0 && name_len < sizeof(name)) {
                memcpy(name, line_start, name_len);
                name[name_len] = '\0';
                if (!is_hop_by_hop_header(name)
                    && !(strip_content_encoding && icmp(name, "Content-Encoding") == 0)) {
                    if (off + line_len + 1 >= cap) {
                        cap = cap + line_len + 1024;
                        char* bigger = (char*)realloc(out, cap);
                        if (!bigger) { free(out); return NULL; }
                        out = bigger;
                    }
                    memcpy(out + off, line_start, line_len);
                    off += line_len;
                    out[off++] = '\n';
                }
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    out[off] = '\0';
    return out;
}

static int header_block_has_token_value(const char* headers, const char* name, const char* token) {
    char* v = response_header_value_from_block(headers, name);
    if (!v) return 0;
    int yes = ascii_ci_contains(v, token);
    free(v);
    return yes;
}

static int request_accepts_gzip(const VcrHttpRequestPrefix* req) {
    const char* ae = request_header_value(req, "Accept-Encoding");
    if (!ae) return 0;
    return ascii_ci_contains(ae, "gzip");
}

static char* response_header_value_from_block(const char* headers, const char* name) {
    if (!headers || !name) return strdup("");
    size_t name_len = strlen(name);
    const char* p = headers;
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p) : strlen(p);
        const char* colon = memchr(p, ':', line_len);
        if (colon && (size_t)(colon - p) == name_len) {
            int match = 1;
            for (size_t i = 0; i < name_len; i++) {
                char a = p[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { match = 0; break; }
            }
            if (match) {
                const char* v = colon + 1;
                const char* end = p + line_len;
                while (v < end && (*v == ' ' || *v == '\t')) v++;
                while (end > v && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
                size_t vlen = (size_t)(end - v);
                char* out = (char*)malloc(vlen + 1);
                if (!out) return NULL;
                memcpy(out, v, vlen);
                out[vlen] = '\0';
                return out;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return strdup("");
}

static void emit_recorded_headers_to_response(void* res, const char* headers) {
    http_response_clear_headers(res);
    if (!headers) return;
    const char* p = headers;
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p) : strlen(p);
        if (line_len > 0 && p[line_len - 1] == '\r') line_len--;
        const char* colon = memchr(p, ':', line_len);
        if (colon) {
            size_t name_len = (size_t)(colon - p);
            const char* val = colon + 1;
            const char* end = p + line_len;
            while (val < end && (*val == ' ' || *val == '\t')) val++;
            size_t val_len = (size_t)(end - val);
            char name[256];
            char value[4096];
            if (name_len > 0 && name_len < sizeof(name) && val_len < sizeof(value)) {
                memcpy(name, p, name_len); name[name_len] = '\0';
                memcpy(value, val, val_len); value[val_len] = '\0';
                http_response_add_header(res, name, value);
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
}

static void vcr_record_dispatch(void* req, void* res, void* ud) {
    (void)ud;
    const VcrHttpRequestPrefix* live_req = (const VcrHttpRequestPrefix*)req;
    if (!g_record_upstream_base || !*g_record_upstream_base) {
        record_dispatch_error(res, 500, "vcr record mode: upstream base not configured");
        return;
    }

    char* url = build_upstream_url(g_record_upstream_base, live_req);
    if (!url) {
        record_dispatch_error(res, 500, "vcr record mode: OOM building upstream URL");
        return;
    }

    void* creq = http_request_raw(live_req->method ? live_req->method : "GET", url);
    if (!creq) {
        free(url);
        record_dispatch_error(res, 500, "vcr record mode: failed to build client request");
        return;
    }

    for (int i = 0; i < live_req->header_count; i++) {
        const char* key = live_req->header_keys[i];
        const char* val = live_req->header_values[i] ? live_req->header_values[i] : "";
        if (!key || is_hop_by_hop_header(key)) continue;
        http_request_set_header_raw(creq, key, val);
    }

    if (live_req->body && live_req->body_length > 0) {
        const char* ctype = request_header_value(live_req, "Content-Type");
        http_request_set_body_raw(creq, live_req->body, (int)live_req->body_length, ctype);
    }
    http_request_set_timeout_raw(creq, 30);

    void* cresp = http_send_raw(creq);
    http_request_free_raw(creq);
    if (!cresp) {
        free(url);
        record_dispatch_error(res, 502, "vcr record mode: upstream request failed");
        return;
    }
    const char* cerr = http_response_error(cresp);
    if (cerr && *cerr) {
        char msg[2048];
        snprintf(msg, sizeof(msg), "vcr record mode: upstream transport error: %s", cerr);
        http_response_free(cresp);
        free(url);
        record_dispatch_error(res, 502, msg);
        return;
    }

    VcrClientResponsePrefix* cr = (VcrClientResponsePrefix*)cresp;
    const char* body = (cr->body && cr->body->data) ? cr->body->data : "";
    int body_len = (cr->body && cr->body->data) ? (int)cr->body->length : 0;
    const char* raw_headers = (cr->headers && cr->headers->data) ? cr->headers->data : "";
    char* caller_headers = response_headers_for_tape(raw_headers, 0);
    int upstream_gzip = header_block_has_token_value(caller_headers ? caller_headers : "", "Content-Encoding", "gzip");
    char* resp_headers = response_headers_for_tape(raw_headers, upstream_gzip);
    char* content_type = response_header_value_from_block(resp_headers ? resp_headers : "", "Content-Type");
    char* req_headers = normalize_live_headers(live_req);

    if (!caller_headers || !resp_headers || !content_type || !req_headers) {
        free(caller_headers); free(resp_headers); free(content_type); free(req_headers);
        http_response_free(cresp);
        free(url);
        record_dispatch_error(res, 500, "vcr record mode: OOM recording interaction");
        return;
    }

    const char* body_for_tape = body;
    char* decoded_body = NULL;
    size_t decoded_body_cap = 0;
    if (upstream_gzip) {
        if (zlib_backend_available() == 0) {
            free(caller_headers); free(resp_headers); free(content_type); free(req_headers);
            http_response_free(cresp);
            free(url);
            record_dispatch_error(res, 500, "vcr record mode: gzip response but zlib unavailable");
            return;
        }
        if (!zlib_try_gzip_inflate(body, body_len)) {
            free(caller_headers); free(resp_headers); free(content_type); free(req_headers);
            http_response_free(cresp);
            free(url);
            record_dispatch_error(res, 500, "vcr record mode: failed to decode gzip response");
            return;
        }
        int body_for_tape_len = zlib_get_inflate_length();
        /* Cap-aware (#343): VCR record-mode decodes the upstream's
         * gzip response into a local copy. Untrusted upstream length
         * drives the allocation; gate via the caps allocator. */
        decoded_body_cap = (size_t)body_for_tape_len + 1;
        decoded_body = (char*)aether_caps_malloc(decoded_body_cap);
        if (!decoded_body) {
            zlib_release_inflate();
            free(caller_headers); free(resp_headers); free(content_type); free(req_headers);
            http_response_free(cresp);
            free(url);
            record_dispatch_error(res, 500, "vcr record mode: OOM decoding gzip response");
            return;
        }
        memcpy(decoded_body, zlib_get_inflate_bytes(), (size_t)body_for_tape_len);
        decoded_body[body_for_tape_len] = '\0';
        zlib_release_inflate();
        body_for_tape = decoded_body;
    }

    char* recorded_path = build_recorded_path(live_req);
    if (!recorded_path) {
        aether_caps_free(decoded_body, decoded_body_cap);
        free(caller_headers); free(resp_headers); free(content_type); free(req_headers);
        http_response_free(cresp);
        free(url);
        record_dispatch_error(res, 500, "vcr record mode: OOM recording path");
        return;
    }

    int ok = vcr_record_interaction_full(live_req->method ? live_req->method : "GET",
                                         recorded_path,
                                         http_response_status(cresp),
                                         content_type,
                                         body_for_tape,
                                         req_headers,
                                         live_req->body ? live_req->body : "");
    if (!ok) {
        aether_caps_free(decoded_body, decoded_body_cap);
        free(caller_headers);
        free(resp_headers);
        free(content_type);
        free(req_headers);
        free(recorded_path);
        http_response_free(cresp);
        free(url);
        record_dispatch_error(res, 500, "vcr record mode: failed to record interaction");
        return;
    }
    vcr_aether_set_resp_headers(g_tape_n - 1, resp_headers);

    http_response_set_status(res, http_response_status(cresp));
    emit_recorded_headers_to_response(res, caller_headers);
    http_response_set_body_n(res, body, body_len);

    last_err_set("");
    g_last_kind = VCR_KIND_OK;
    g_last_index = g_tape_n - 1;

    aether_caps_free(decoded_body, decoded_body_cap);
    free(caller_headers);
    free(resp_headers);
    free(content_type);
    free(req_headers);
    free(recorded_path);
    http_response_free(cresp);
    free(url);
}

void vcr_dispatch(void* req, void* res, void* ud) {
    (void)ud;
    if (g_tape_cursor >= g_tape_n) {
        last_err_set("tape exhausted — SUT made more requests than the tape contains");
        g_last_kind = VCR_KIND_TAPE_EXHAUSTED;
        g_last_index = g_tape_cursor;
        http_response_set_status(res, 599);
        http_response_set_body(res,
            "tape exhausted — SUT made more requests than the tape contains");
        return;
    }
    Interaction* e = &g_tape[g_tape_cursor];
    char* expected_path = apply_unredactions_for_c(e->path, VCR_FIELD_PATH);
    char* expected_req_headers = apply_unredactions_for_c(e->req_headers, VCR_FIELD_REQUEST_HEADERS);
    char* expected_req_body = apply_unredactions_for_c(e->req_body, VCR_FIELD_REQUEST_BODY);
    if (!expected_path || !expected_req_headers || !expected_req_body) {
        free(expected_path); free(expected_req_headers); free(expected_req_body);
        last_err_set("OOM applying playback replacements");
        http_response_set_status(res, 599);
        http_response_set_body(res, "OOM applying playback replacements");
        return;
    }
    char* expected_req_headers_filtered = remove_headers_from_block_c(expected_req_headers,
                                                                      VCR_FIELD_REQUEST_HEADERS);
    free(expected_req_headers);
    expected_req_headers = expected_req_headers_filtered;
    if (!expected_req_headers) {
        free(expected_path); free(expected_req_body);
        last_err_set("OOM applying playback header removals");
        http_response_set_status(res, 599);
        http_response_set_body(res, "OOM applying playback header removals");
        return;
    }

    /* (method, path) match — same as before step 10, just now
     * recorded into g_last_error too. Mismatch returns without
     * advancing cursor. */
    const char* got_method = http_request_method(req);
    const char* got_path   = http_request_path(req);
    char* got_path_with_query = NULL;
    if (expected_path_includes_query(expected_path)) {
        got_path_with_query = build_recorded_path((const VcrHttpRequestPrefix*)req);
        got_path = got_path_with_query ? got_path_with_query : got_path;
    }
    if (!got_method || strcmp(got_method, e->method) != 0
        || !got_path || strcmp(got_path, expected_path) != 0) {
        char msg[2048];
        snprintf(msg, sizeof(msg),
            "tape mismatch at interaction %d: expected %s %s, got %s %s",
            g_tape_cursor,
            e->method, expected_path,
            got_method ? got_method : "(null)",
            got_path   ? got_path   : "(null)");
        last_err_set(msg);
        g_last_kind = VCR_KIND_PATH_OR_METHOD_DIFF;
        g_last_index = g_tape_cursor;
        http_response_set_status(res, 599);
        http_response_set_body(res, msg);
        free(got_path_with_query);
        free(expected_path); free(expected_req_headers); free(expected_req_body);
        return;
    }
    free(got_path_with_query);

    /* Request-headers comparison. Fires when EITHER the tape captured
     * a non-blank request_headers block (implicit gate, Step 10) OR
     * the test explicitly opted in via vcr.set_strict_headers(1). The
     * explicit flag lets a test enable strict matching against tapes
     * whose request blocks have been load-time scrubbed. Recorded
     * upstream Host headers won't match what std.http.client sends to
     * 127.0.0.1, but a test driver that wants to deliberately exercise
     * the diagnostic paths can scrub-then-set-strict and construct
     * exact requests. */
    const VcrHttpRequestPrefix* live_req = (const VcrHttpRequestPrefix*)req;
    if (!is_blank(expected_req_headers) || g_strict_headers) {
        char* live = normalize_live_headers(live_req);
        if (!live) {
            free(expected_path); free(expected_req_headers); free(expected_req_body);
            last_err_set("OOM normalizing live request headers");
            http_response_set_status(res, 599);
            http_response_set_body(res, "OOM normalizing live request headers");
            return;
        }
        if (strcmp(live, expected_req_headers) != 0) {
            /* Identify the offending header. Walk the recorded blob
             * line by line: if a recorded line isn't in `live`, the
             * recording expected a header the SUT didn't send →
             * "<name> request header was expected but not encountered".
             * If we find no missing-from-live recorded line, walk live
             * for an unexpected one → "<name> request header
             * encountered but not expected". For same-name different-
             * value, → "<name> request header value differed". */
            char hdr_name[128];
            char msg[2048];
            int found_diag = 0;
            int kind = VCR_KIND_HEADER_MISSING;  /* default if neither sweep narrows it */

            /* First sweep: recorded lines not present verbatim in live. */
            const char* p = expected_req_headers;
            while (*p && !found_diag) {
                const char* eol = strchr(p, '\n');
                if (!eol) eol = p + strlen(p);
                size_t llen = (size_t)(eol - p);
                if (llen > 0) {
                    /* Reconstruct "<line>\n" so we match a full recorded line
                     * inside the live blob (live ends every line with \n). */
                    char one[512];
                    if (llen < sizeof(one) - 2) {
                        memcpy(one, p, llen);
                        one[llen] = '\n';
                        one[llen + 1] = '\0';
                        if (!strstr(live, one)) {
                            /* This recorded header is missing from live.
                             * Check whether the same NAME appears with a
                             * different value (value-differed) vs missing
                             * outright. */
                            const char* colon = (const char*)memchr(p, ':', llen);
                            if (colon) {
                                size_t nlen = (size_t)(colon - p);
                                if (nlen + 2 < sizeof(one)) {
                                    memcpy(one, p, nlen);
                                    one[nlen] = ':';
                                    one[nlen + 1] = '\0';
                                    /* Live blob always renders "Name: ..." */
                                    char* maybe = strstr(live, one);
                                    if (maybe && (maybe == live || maybe[-1] == '\n')) {
                                        first_header_name(p, hdr_name, sizeof(hdr_name));
                                        snprintf(msg, sizeof(msg),
                                            "interaction %d: '%s' request header value differed",
                                            g_tape_cursor, hdr_name);
                                        kind = VCR_KIND_HEADER_VALUE_DIFF;
                                        found_diag = 1;
                                        break;
                                    }
                                }
                            }
                            first_header_name(p, hdr_name, sizeof(hdr_name));
                            snprintf(msg, sizeof(msg),
                                "interaction %d: '%s' request header was expected but not encountered",
                                g_tape_cursor, hdr_name);
                            kind = VCR_KIND_HEADER_MISSING;
                            found_diag = 1;
                        }
                    }
                }
                if (!*eol) break;
                p = eol + 1;
            }

            /* Second sweep: live lines not in recorded → unexpected. */
            if (!found_diag) {
                const char* lp = live;
                while (*lp && !found_diag) {
                    const char* eol = strchr(lp, '\n');
                    if (!eol) eol = lp + strlen(lp);
                    size_t llen = (size_t)(eol - lp);
                    if (llen > 0 && llen < 510) {
                        char one[512];
                        memcpy(one, lp, llen);
                        one[llen] = '\n';
                        one[llen + 1] = '\0';
                        if (!strstr(expected_req_headers, one)) {
                            first_header_name(lp, hdr_name, sizeof(hdr_name));
                            snprintf(msg, sizeof(msg),
                                "interaction %d: '%s' request header encountered but not expected",
                                g_tape_cursor, hdr_name);
                            kind = VCR_KIND_HEADER_UNEXPECTED;
                            found_diag = 1;
                        }
                    }
                    if (!*eol) break;
                    lp = eol + 1;
                }
            }

            if (!found_diag) {
                snprintf(msg, sizeof(msg),
                    "interaction %d: request headers differ — recorded:\n%s\nlive:\n%s",
                    g_tape_cursor, expected_req_headers, live);
                /* Fall-through diagnostic — neither sweep located a
                 * single offender. Call it MISSING by default; the
                 * detailed message is in g_last_error. */
                kind = VCR_KIND_HEADER_MISSING;
            }
            last_err_set(msg);
            g_last_kind = kind;
            g_last_index = g_tape_cursor;
            free(live);
            free(expected_path); free(expected_req_headers); free(expected_req_body);
            http_response_set_status(res, 599);
            http_response_set_body(res, msg);
            return;
        }
        free(live);
    }

    /* Step 10: request-body comparison. Same opt-in semantics. */
    if (!is_blank(expected_req_body)) {
        const char* live_body = live_req->body ? live_req->body : "";
        if (strcmp(live_body, expected_req_body) != 0) {
            char msg[2048];
            snprintf(msg, sizeof(msg),
                "interaction %d: request body different to expectation — "
                "recorded=%zu bytes, live=%zu bytes",
                g_tape_cursor,
                strlen(expected_req_body),
                strlen(live_body));
            last_err_set(msg);
            g_last_kind = VCR_KIND_BODY_DIFF;
            g_last_index = g_tape_cursor;
            free(expected_path); free(expected_req_headers); free(expected_req_body);
            http_response_set_status(res, 599);
            http_response_set_body(res, msg);
            return;
        }
    }

    http_response_set_status(res, e->status);

    /* Wipe defaults from http_response_create (Content-Type:
     * text/html, Server: Aether/1.0). Servirtium-spec replay must
     * serve EXACTLY what was recorded, with no Aether overlays —
     * otherwise the wire bytes don't match what the original server
     * sent and clients that care about response-header ordering
     * (or repeated keys, or specific Server identity) reject the
     * response. */
    http_response_clear_headers(res);

    /* Walk the tape's `### Response headers recorded for playback:`
     * block and emit each line verbatim, in order. Includes
     * Content-Type if the tape recorded one (which it does, in
     * canonical Servirtium markdown — the parser stores the body's
     * `(<status>: <ct>)` opener AND the resp_headers block; both
     * will normally agree). Tapes pre-dating the resp_headers
     * field fall back to the `e->content_type` emission below. */
    int emitted_content_type_from_block = 0;
    if (e->resp_headers && e->resp_headers[0]) {
        const char* p = e->resp_headers;
        while (*p) {
            const char* line_start = p;
            while (*p && *p != '\n') p++;
            size_t line_len = (size_t)(p - line_start);
            if (*p == '\n') p++;
            if (line_len == 0) continue;
            /* Strip a trailing \r if present (CRLF tolerance). */
            if (line_len > 0 && line_start[line_len - 1] == '\r') line_len--;
            if (line_len == 0) continue;
            /* Find the colon. Skip lines without one — defensive
             * against malformed tape blocks; better to drop a line
             * than to set a junk header. */
            const char* colon = NULL;
            for (size_t i = 0; i < line_len; i++) {
                if (line_start[i] == ':') { colon = line_start + i; break; }
            }
            if (!colon) continue;
            size_t name_len = (size_t)(colon - line_start);
            if (name_len == 0) continue;
            /* Value starts after ': ' or ':'; skip ASCII whitespace. */
            const char* val_start = colon + 1;
            const char* line_end = line_start + line_len;
            while (val_start < line_end && (*val_start == ' ' || *val_start == '\t')) val_start++;
            size_t val_len = (size_t)(line_end - val_start);
            /* Stack-bounded copies — header names are bounded in
             * practice (RFC 7230 doesn't specify a hard cap; 256 is
             * the conventional engineering ceiling). Drop the
             * line if it's longer rather than truncate-and-set. */
            char name_buf[256];
            char val_buf[4096];
            if (name_len >= sizeof(name_buf)) continue;
            if (val_len  >= sizeof(val_buf))  continue;
            memcpy(name_buf, line_start, name_len);
            name_buf[name_len] = '\0';
            memcpy(val_buf, val_start, val_len);
            val_buf[val_len] = '\0';
            /* Track whether the tape included a Content-Type line so
             * we don't double-emit one from e->content_type below. */
            if (name_len == 12) {
                int matches_ct = 1;
                const char* ct = "Content-Type";
                for (size_t i = 0; i < 12; i++) {
                    char a = name_buf[i];
                    char b = ct[i];
                    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                    if (a != b) { matches_ct = 0; break; }
                }
                if (matches_ct) emitted_content_type_from_block = 1;
            }
            /* add_header (not set_header) — some protocols have many
             * duplicate-keyed headers. set_header replaces on duplicate;
             * add_header preserves order and multiplicity through to
             * the wire serializer. */
            if (icmp(name_buf, "Content-Length") != 0
                && icmp(name_buf, "Content-Encoding") != 0) {
                http_response_add_header(res, name_buf, val_buf);
            }
        }
    }
    /* Fallback: tapes without a resp_headers block (or with one that
     * happens to omit Content-Type) still need a Content-Type so the
     * client knows how to interpret the body. The body's response
     * line is the canonical source for these legacy tapes. */
    if (!emitted_content_type_from_block && e->content_type && e->content_type[0]) {
        http_response_add_header(res, "Content-Type", e->content_type);
    }
    if (ascii_ci_contains(e->content_type, "base64 below")) {
        unsigned char* decoded = NULL;
        int decoded_len = 0;
        if (decode_base64_body(e->body, &decoded, &decoded_len)) {
            http_response_set_body_n(res, (const char*)decoded, decoded_len);
            free(decoded);
        } else {
            http_response_set_body(res, e->body);
        }
    } else {
        if (request_accepts_gzip(live_req) && zlib_backend_available() == 1
            && zlib_try_gzip_deflate(e->body ? e->body : "", (int)strlen(e->body ? e->body : ""), -1)) {
            const char* gz = zlib_get_deflate_bytes();
            int gz_len = zlib_get_deflate_length();
            http_response_set_body_n(res, gz, gz_len);
            zlib_release_deflate();
            http_response_set_header(res, "Content-Encoding", "gzip");
            http_response_set_header(res, "Vary", "Accept-Encoding");
        } else {
            http_response_set_body(res, e->body);
        }
    }

    /* Successful dispatch — clear any prior diagnostic and stamp
     * the per-dispatch outcome slots. The next request the test
     * makes overwrites; tests are responsible for reading after
     * each request before the next one fires. */
    last_err_set("");
    g_last_kind = VCR_KIND_OK;
    g_last_index = g_tape_cursor;

    g_tape_cursor++;
    free(expected_path); free(expected_req_headers); free(expected_req_body);
}

/* Step 10: surface the most recent dispatch mismatch. The test's
 * tearDown hook calls vcr.last_error() to confirm the right thing
 * happened (or empty, if no mismatch occurred this run).
 * Returns "" when nothing has been flagged. */
const char* vcr_last_error(void) {
    return g_last_error ? g_last_error : "";
}

/* Clear the last-error slot. Call between subtests in the same
 * process so a flagged mismatch doesn't bleed across them. */
void vcr_clear_last_error(void) {
    free(g_last_error);
    g_last_error = NULL;
    g_last_kind = VCR_KIND_OK;
    g_last_index = -1;
}

/* Per-dispatch outcome read accessors. Tests check these after each
 * request to confirm the dispatcher accepted (or rejected, with
 * which kind) the SUT's request. See VCR_KIND_* defines above. */
int vcr_last_kind(void) { return g_last_kind; }
int vcr_last_index(void) { return g_last_index; }

/* Opt-in strict-headers matching. When set to 1, the dispatcher
 * compares the SUT's request headers against the recorded block
 * unconditionally — even if the recorded block is blank. Used by
 * tests that want to deliberately exercise the diagnostic paths
 * (negative-path coverage). Default is 0 (the implicit gate from
 * Step 10 still works either way). */
void vcr_set_strict_headers(int on) { g_strict_headers = on ? 1 : 0; }
int vcr_get_strict_headers(void) { return g_strict_headers; }

/* Reset the dispatch cursor to interaction 0 without freeing the
 * tape or stopping the server. Used by tests that drive a series
 * of independent cases against the same loaded tape — each case
 * resets the cursor at the top, sends its request, asserts. */
void vcr_reset_cursor(void) {
    g_tape_cursor = 0;
    free(g_last_error);
    g_last_error = NULL;
    g_last_kind = VCR_KIND_OK;
    g_last_index = -1;
}

/* ---- Tape-iteration accessors (Aether-side emitter consumes these) ----
 *
 * The emitter is being ported to Aether. These accessors let the
 * Aether code walk the in-memory tape one Interaction at a time
 * without crossing a struct boundary. Each returns "" (not NULL)
 * for absent / empty fields so the Aether side can do flat string
 * concatenation without null guards. Notes return NULL-equivalent
 * empty strings; vcr_get_note_present() distinguishes "no note"
 * from "note with empty body". Same for content_type. */

int vcr_get_tape_count(void) { return g_tape_n; }

static const char* safe(const char* s) { return s ? s : ""; }

const char* vcr_get_method(int i) {
    if (i < 0 || i >= g_tape_n) return "";
    return safe(g_tape[i].method);
}
const char* vcr_get_path(int i) {
    if (i < 0 || i >= g_tape_n) return "";
    return safe(g_tape[i].path);
}
int vcr_get_status(int i) {
    if (i < 0 || i >= g_tape_n) return 0;
    return g_tape[i].status;
}
const char* vcr_get_content_type(int i) {
    if (i < 0 || i >= g_tape_n) return "";
    return safe(g_tape[i].content_type);
}
const char* vcr_get_body(int i) {
    if (i < 0 || i >= g_tape_n) return "";
    return safe(g_tape[i].body);
}
const char* vcr_get_req_headers(int i) {
    if (i < 0 || i >= g_tape_n) return "";
    return safe(g_tape[i].req_headers);
}
const char* vcr_get_req_body(int i) {
    if (i < 0 || i >= g_tape_n) return "";
    return safe(g_tape[i].req_body);
}
const char* vcr_get_resp_headers(int i) {
    if (i < 0 || i >= g_tape_n) return "";
    return safe(g_tape[i].resp_headers);
}
int vcr_get_note_present(int i) {
    if (i < 0 || i >= g_tape_n) return 0;
    return g_tape[i].note_title ? 1 : 0;
}
const char* vcr_get_note_title(int i) {
    if (i < 0 || i >= g_tape_n) return "";
    return safe(g_tape[i].note_title);
}
const char* vcr_get_note_body(int i) {
    if (i < 0 || i >= g_tape_n) return "";
    return safe(g_tape[i].note_body);
}

/* Toggle accessors — the Aether emitter reads these when deciding
 * which markdown form to emit. */
int vcr_get_indent_code_blocks(void)   { return g_indent_code_blocks; }
int vcr_get_emphasize_http_verbs(void) { return g_emphasize_http_verbs; }

/* ---- Tape setters (Aether-side parser populates the tape via these) ----
 *
 * The parser is being ported to Aether. These setters let the Aether
 * code build up the in-memory tape without crossing the struct
 * boundary. Single all-fields setter avoids per-field setters
 * (which would need a "current interaction" cursor). Empty strings
 * for absent fields — the existing tape_append takes "" the same
 * way and the dispatcher skips matching when fields are empty.
 *
 * vcr_aether_clear_tape resets storage so the parser can rebuild
 * cleanly; it's the equivalent of what vcr_load_tape does at the
 * top of its body before parsing. */
void vcr_aether_clear_tape(void) {
    tape_free_storage();
    /* vcr_load_tape sets g_tape_err to "" so vcr_load_err returns
     * an empty string when called between a successful load and
     * the next parse failure. Mirror that behavior here. */
    g_tape_err = strdup("");
}

int vcr_aether_append_interaction(const char* method, const char* path,
                                  int status, const char* content_type,
                                  const char* body,
                                  const char* req_headers, const char* req_body,
                                  const char* note_title, const char* note_body) {
    /* The pending-note slot (used by record-mode) must NOT be
     * drained by the parser — loaded tapes carry their own per-
     * interaction notes. We stage the note pair into the slot
     * directly so tape_append's drain logic places it on the
     * about-to-be-appended interaction. */
    if (note_title && *note_title) {
        free(g_pending_note_title);
        free(g_pending_note_body);
        g_pending_note_title = strdup(note_title);
        g_pending_note_body  = strdup(note_body ? note_body : "");
    }
    /* Empty content_type → store NULL so the dispatcher's existing
     * "if (e->content_type)" branch behaves correctly. */
    const char* ct = (content_type && *content_type) ? content_type : NULL;
    if (tape_append(method, path, status, body, ct,
                    req_headers ? req_headers : "",
                    req_body    ? req_body    : "") != 0) {
        tape_set_err("OOM appending parsed interaction");
        return 0;
    }
    return 1;
}

/* Set the load error string. Aether parser calls this when it
 * detects a malformed tape so vcr.load_err() surfaces a useful
 * diagnostic. */
void vcr_aether_set_load_err(const char* msg) {
    tape_set_err(msg ? msg : "");
}

/* Attach a response-headers block to the most recently appended
 * interaction (i.e. the entry vcr_aether_append_interaction just
 * created). Aether parser calls this after detecting a
 * `### Response headers recorded for playback:` block in the tape;
 * the dispatcher walks the stored value at replay time and emits
 * each line as a real response header via http_response_set_header.
 *
 * Empty / NULL value is a no-op (matches the legacy "no response
 * headers in tape, dispatcher emits only Content-Type" behaviour).
 *
 * Per-index variant rather than "set on the most recent" because
 * the parser already passes the interaction index via the same
 * walking loop; explicit-index avoids any ordering surprise if
 * the parser is restructured later. */
void vcr_aether_set_resp_headers(int index, const char* headers) {
    if (index < 0 || index >= g_tape_n) return;
    if (!headers) headers = "";
    free(g_tape[index].resp_headers);
    g_tape[index].resp_headers = strdup(headers);
}

/* Redaction iteration — the Aether emitter applies redactions
 * before writing path / response-body fields. */
int vcr_get_redaction_count(void) { return g_redactions_n; }
int vcr_get_redaction_field(int i) {
    if (i < 0 || i >= g_redactions_n) return 0;
    return g_redactions[i].field;
}
const char* vcr_get_redaction_pattern(int i) {
    if (i < 0 || i >= g_redactions_n) return "";
    return safe(g_redactions[i].pattern);
}
const char* vcr_get_redaction_replacement(int i) {
    if (i < 0 || i >= g_redactions_n) return "";
    return safe(g_redactions[i].replacement);
}
int vcr_get_unredaction_count(void) { return g_unredactions_n; }
int vcr_get_unredaction_field(int i) {
    if (i < 0 || i >= g_unredactions_n) return 0;
    return g_unredactions[i].field;
}
const char* vcr_get_unredaction_pattern(int i) {
    if (i < 0 || i >= g_unredactions_n) return "";
    return safe(g_unredactions[i].pattern);
}
const char* vcr_get_unredaction_replacement(int i) {
    if (i < 0 || i >= g_unredactions_n) return "";
    return safe(g_unredactions[i].replacement);
}
int vcr_get_header_removal_count(void) { return g_header_removals_n; }
int vcr_get_header_removal_field(int i) {
    if (i < 0 || i >= g_header_removals_n) return 0;
    return g_header_removals[i].field;
}
const char* vcr_get_header_removal_name(int i) {
    if (i < 0 || i >= g_header_removals_n) return "";
    return safe(g_header_removals[i].name);
}
