/* Shim that constructs HttpRequest / HttpServerResponse on the C side
 * and hands pointers to Aether. Mirrors what a C dispatch layer would
 * do when it owns the HTTP parse/serialize loop and wants an Aether
 * function to fill in handler logic.
 *
 * Field layout MUST match std/net/aether_http_server.h. If the runtime
 * struct layout changes, this test catches it.
 */

#include <stdlib.h>
#include <string.h>

typedef struct {
    char* method;
    char* path;
    char* query_string;
    char* http_version;
    char** header_keys;
    char** header_values;
    int header_count;
    char* body;
    size_t body_length;
    char** param_keys;
    char** param_values;
    int param_count;
    /* #625: lazily-parsed query-param cache. Tracked at the end of
     * the struct so existing C consumers stay binary-compatible if
     * they fail to update — but the calloc below sets everything to
     * zero (query_parsed=0 = "not yet attempted"), which is what
     * http_get_query_param's lazy-parse path expects. */
    char** query_keys;
    char** query_values;
    int query_count;
    int query_parsed;
} HttpRequest;

typedef struct {
    int status_code;
    char* status_text;
    char** header_keys;
    char** header_values;
    int header_count;
    char* body;
    size_t body_length;
} HttpServerResponse;

void* probe_make_request(void) {
    HttpRequest* r = calloc(1, sizeof(*r));
    r->method       = strdup("GET");
    r->path         = strdup("/api/foo");
    r->query_string = strdup("x=1&y=two");
    r->http_version = strdup("HTTP/1.1");
    r->header_keys   = calloc(2, sizeof(char*));
    r->header_values = calloc(2, sizeof(char*));
    r->header_keys[0] = strdup("X-Test");   r->header_values[0] = strdup("hi");
    r->header_keys[1] = strdup("X-Other");  r->header_values[1] = strdup("bye");
    r->header_count = 2;
    r->body = strdup("");
    r->body_length = 0;
    return r;
}

/* An intentionally-bare response: status_text, header_keys,
 * header_values, body all NULL. http_response_set_* must tolerate this
 * and lazy-allocate, otherwise a C dispatch layer calling into Aether
 * handlers to populate the response is guaranteed to crash on the
 * first setter call. */
void* probe_make_bare_response(void) {
    return calloc(1, sizeof(HttpServerResponse));
}

int         probe_res_status(void* p) { return ((HttpServerResponse*)p)->status_code; }
const char* probe_res_body(void* p)   {
    HttpServerResponse* r = p;
    return r->body ? r->body : "";
}
int         probe_res_header_count(void* p) {
    return ((HttpServerResponse*)p)->header_count;
}

void probe_free_request(void* p) {
    HttpRequest* r = p;
    if (!r) return;
    free(r->method); free(r->path); free(r->query_string); free(r->http_version);
    for (int i = 0; i < r->header_count; i++) {
        free(r->header_keys[i]); free(r->header_values[i]);
    }
    free(r->header_keys); free(r->header_values);
    free(r->body);
    /* Mirror http_request_free's query-cache cleanup — set by the
     * lazy parse path inside http_get_query_param the first time
     * the test calls it. */
    for (int i = 0; i < r->query_count; i++) {
        free(r->query_keys[i]); free(r->query_values[i]);
    }
    free(r->query_keys); free(r->query_values);
    free(r);
}

void probe_free_response(void* p) {
    HttpServerResponse* r = p;
    if (!r) return;
    free(r->status_text);
    for (int i = 0; i < r->header_count; i++) {
        free(r->header_keys[i]); free(r->header_values[i]);
    }
    free(r->header_keys); free(r->header_values);
    free(r->body);
    free(r);
}
