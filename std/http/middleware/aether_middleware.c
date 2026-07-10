// std.http.middleware — pre-handler middleware primitives.
// See aether_middleware.h for the API contract.
//
// Each middleware ships with:
//   - An options struct (config) the user constructs once at startup.
//   - A C function with the existing HttpMiddleware signature
//     `int(req, res, user_data) -> int`, registered via
//     http_server_use_middleware. Returns 1 to continue the chain,
//     0 to short-circuit.
//
// The hot path is straight C function pointers — no closure
// indirection, no Aether-side dispatch. Aether-side factory
// wrappers in std/http/middleware/module.ae allocate the user_data
// and register the middleware with the server.
#include "aether_middleware.h"
#include "../../../runtime/aether_resource_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
// pthread types (pthread_mutex_t / lock / unlock / init / destroy) come
// in transitively via aether_middleware.h -> aether_http_server.h ->
// multicore_scheduler.h -> runtime/utils/aether_thread.h. That header
// provides a Win32 shim on MinGW; including <pthread.h> directly here
// would conflict with that shim. Do NOT add #include <pthread.h>.

// Borrow string accessors from the existing http surface.
extern const char* http_get_header(HttpRequest*, const char*);
extern void        http_response_set_status(HttpServerResponse*, int);
extern void        http_response_set_header(HttpServerResponse*, const char*, const char*);
extern void        http_response_set_body  (HttpServerResponse*, const char*);
extern const char* http_request_method(HttpRequest*);
extern const char* http_request_path  (HttpRequest*);

// -----------------------------------------------------------------
// CORS
// -----------------------------------------------------------------
struct AetherCorsOpts {
    char* allow_origin;       /* NULL/"" -> don't emit */
    char* allow_methods;
    char* allow_headers;
    int   allow_credentials;
    int   max_age_seconds;    /* <= 0 -> don't emit Max-Age */
};

static char* dup_or_null(const char* s) {
    if (!s || !*s) return NULL;
    return strdup(s);
}

AetherCorsOpts* aether_cors_opts_new(const char* allow_origin,
                                     const char* allow_methods,
                                     const char* allow_headers,
                                     int allow_credentials,
                                     int max_age_seconds) {
    AetherCorsOpts* o = (AetherCorsOpts*)calloc(1, sizeof(AetherCorsOpts));
    if (!o) return NULL;
    o->allow_origin      = dup_or_null(allow_origin);
    o->allow_methods     = dup_or_null(allow_methods);
    o->allow_headers     = dup_or_null(allow_headers);
    o->allow_credentials = allow_credentials ? 1 : 0;
    o->max_age_seconds   = max_age_seconds;
    return o;
}

void aether_cors_opts_free(AetherCorsOpts* o) {
    if (!o) return;
    free(o->allow_origin);
    free(o->allow_methods);
    free(o->allow_headers);
    free(o);
}

int aether_middleware_cors(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    AetherCorsOpts* o = (AetherCorsOpts*)user_data;
    if (!o) return 1;

    /* CORS headers go on every response, not just preflight. */
    if (o->allow_origin)  http_response_set_header(res, "Access-Control-Allow-Origin",  o->allow_origin);
    if (o->allow_methods) http_response_set_header(res, "Access-Control-Allow-Methods", o->allow_methods);
    if (o->allow_headers) http_response_set_header(res, "Access-Control-Allow-Headers", o->allow_headers);
    if (o->allow_credentials)
        http_response_set_header(res, "Access-Control-Allow-Credentials", "true");

    /* Preflight OPTIONS — answer with 204 + Max-Age and short-circuit
     * the chain. The route handler never runs for preflight requests. */
    const char* method = http_request_method(req);
    if (method && strcasecmp(method, "OPTIONS") == 0) {
        if (o->max_age_seconds > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", o->max_age_seconds);
            http_response_set_header(res, "Access-Control-Max-Age", buf);
        }
        http_response_set_status(res, 204);
        http_response_set_body(res, "");
        return 0;  /* short-circuit */
    }
    return 1;  /* continue chain */
}

// -----------------------------------------------------------------
// Basic Authentication
// -----------------------------------------------------------------
struct AetherBasicAuthOpts {
    char* realm;
    AetherBasicAuthVerifier verify;
    void* verifier_user_data;
};

AetherBasicAuthOpts* aether_basic_auth_opts_new(const char* realm,
                                                AetherBasicAuthVerifier verify,
                                                void* verifier_user_data) {
    if (!verify) return NULL;
    AetherBasicAuthOpts* o = (AetherBasicAuthOpts*)calloc(1, sizeof(AetherBasicAuthOpts));
    if (!o) return NULL;
    o->realm = realm && *realm ? strdup(realm) : strdup("Restricted");
    o->verify = verify;
    o->verifier_user_data = verifier_user_data;
    return o;
}

void aether_basic_auth_opts_free(AetherBasicAuthOpts* o) {
    if (!o) return;
    free(o->realm);
    free(o);
}

/* RFC 4648 base64 alphabet. Decoding tolerant of whitespace + padding. */
static int b64_decode_char(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode a base64 string. Caller frees. Returns NULL on malformed
 * input. *out_len receives the decoded byte count. */
static char* b64_decode(const char* src, size_t* out_len) {
    size_t src_len = strlen(src);
    char* out = (char*)malloc(src_len + 1);
    if (!out) return NULL;
    int bits = 0, val = 0;
    size_t out_pos = 0;
    for (size_t i = 0; i < src_len; i++) {
        int c = (unsigned char)src[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int d = b64_decode_char(c);
        if (d < 0) { free(out); return NULL; }
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[out_pos++] = (char)((val >> bits) & 0xff);
        }
    }
    out[out_pos] = '\0';
    *out_len = out_pos;
    return out;
}

int aether_middleware_basic_auth(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    AetherBasicAuthOpts* o = (AetherBasicAuthOpts*)user_data;
    if (!o || !o->verify) return 1;

    const char* auth = http_get_header(req, "Authorization");
    if (!auth || strncasecmp(auth, "Basic ", 6) != 0) {
        char hdr[256];
        snprintf(hdr, sizeof(hdr), "Basic realm=\"%s\"", o->realm);
        http_response_set_header(res, "WWW-Authenticate", hdr);
        http_response_set_status(res, 401);
        http_response_set_body(res, "Unauthorized");
        return 0;
    }

    const char* b64 = auth + 6;
    while (*b64 == ' ' || *b64 == '\t') b64++;

    size_t dec_len = 0;
    char* decoded = b64_decode(b64, &dec_len);
    if (!decoded) {
        http_response_set_status(res, 400);
        http_response_set_body(res, "Bad Authorization header");
        return 0;
    }
    char* sep = strchr(decoded, ':');
    if (!sep) {
        free(decoded);
        http_response_set_status(res, 400);
        http_response_set_body(res, "Bad Authorization payload");
        return 0;
    }
    *sep = '\0';
    const char* user = decoded;
    const char* pass = sep + 1;

    int ok = o->verify(user, pass, o->verifier_user_data);
    free(decoded);

    if (!ok) {
        char hdr[256];
        snprintf(hdr, sizeof(hdr), "Basic realm=\"%s\"", o->realm);
        http_response_set_header(res, "WWW-Authenticate", hdr);
        http_response_set_status(res, 401);
        http_response_set_body(res, "Unauthorized");
        return 0;
    }
    return 1;  /* authorised — continue */
}

// -----------------------------------------------------------------
// Bearer Token Authentication (RFC 6750)
// -----------------------------------------------------------------
struct AetherBearerAuthOpts {
    char* realm;
    AetherBearerAuthVerifier verify;
    void* verifier_user_data;
};

AetherBearerAuthOpts* aether_bearer_auth_opts_new(const char* realm,
                                                  AetherBearerAuthVerifier verify,
                                                  void* verifier_user_data) {
    if (!verify) return NULL;
    AetherBearerAuthOpts* o = (AetherBearerAuthOpts*)calloc(1, sizeof(AetherBearerAuthOpts));
    if (!o) return NULL;
    o->realm = realm && *realm ? strdup(realm) : strdup("api");
    o->verify = verify;
    o->verifier_user_data = verifier_user_data;
    return o;
}

void aether_bearer_auth_opts_free(AetherBearerAuthOpts* o) {
    if (!o) return;
    free(o->realm);
    free(o);
}

/* Per RFC 6750 §3.1: the Bearer challenge can carry an `error`
 * parameter that distinguishes "no credentials" from "bad
 * credentials". Conforming clients (curl, OAuth libraries) use
 * this to decide whether to retry vs. prompt the user. */
static void bearer_challenge(HttpServerResponse* res,
                             const char* realm,
                             const char* error) {
    char hdr[512];
    if (error && *error) {
        snprintf(hdr, sizeof(hdr),
                 "Bearer realm=\"%s\", error=\"%s\"", realm, error);
    } else {
        snprintf(hdr, sizeof(hdr), "Bearer realm=\"%s\"", realm);
    }
    http_response_set_header(res, "WWW-Authenticate", hdr);
    http_response_set_status(res, 401);
    http_response_set_body(res, "Unauthorized");
}

int aether_middleware_bearer_auth(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    AetherBearerAuthOpts* o = (AetherBearerAuthOpts*)user_data;
    if (!o || !o->verify) return 1;

    const char* auth = http_get_header(req, "Authorization");
    if (!auth || strncasecmp(auth, "Bearer ", 7) != 0) {
        /* No credential — base challenge, no `error` param. */
        bearer_challenge(res, o->realm, NULL);
        return 0;
    }

    /* Skip the "Bearer " prefix and any whitespace. The token itself
     * is RFC-defined as 1*( ALPHA / DIGIT / "-" / "." / "_" / "~" /
     * "+" / "/" ) [ "=" *"=" ] but we don't enforce — verifiers
     * commonly accept tighter or looser shapes (JWTs are dotted,
     * opaque tokens are URL-safe base64, etc.). */
    const char* token = auth + 7;
    while (*token == ' ' || *token == '\t') token++;

    if (!*token) {
        /* "Bearer" with no token. Treat as malformed credential. */
        bearer_challenge(res, o->realm, "invalid_token");
        return 0;
    }

    int ok = o->verify(token, o->verifier_user_data);
    if (!ok) {
        bearer_challenge(res, o->realm, "invalid_token");
        return 0;
    }
    return 1;
}

// -----------------------------------------------------------------
// Session-Cookie Authentication
// -----------------------------------------------------------------
struct AetherSessionAuthOpts {
    char* cookie_name;
    char* redirect_url;          /* NULL -> 401, non-NULL -> 302 */
    AetherSessionAuthVerifier verify;
    void* verifier_user_data;
};

AetherSessionAuthOpts* aether_session_auth_opts_new(const char* cookie_name,
                                                    const char* redirect_url,
                                                    AetherSessionAuthVerifier verify,
                                                    void* verifier_user_data) {
    if (!verify || !cookie_name || !*cookie_name) return NULL;
    AetherSessionAuthOpts* o = (AetherSessionAuthOpts*)calloc(1, sizeof(AetherSessionAuthOpts));
    if (!o) return NULL;
    o->cookie_name = strdup(cookie_name);
    // cookie_name is compared on every request; a NULL would crash that lookup.
    // (redirect_url is optional, so a NULL there is a valid "no redirect".)
    if (!o->cookie_name) { free(o); return NULL; }
    o->redirect_url = (redirect_url && *redirect_url) ? strdup(redirect_url) : NULL;
    o->verify = verify;
    o->verifier_user_data = verifier_user_data;
    return o;
}

void aether_session_auth_opts_free(AetherSessionAuthOpts* o) {
    if (!o) return;
    free(o->cookie_name);
    free(o->redirect_url);
    free(o);
}

/* Extract the value of cookie `name` from a Cookie: header value.
 * Cookie syntax (RFC 6265 §4.2.1): `name1=val1; name2=val2; ...`.
 * Names match case-sensitively per RFC 6265. Cookie values can
 * contain anything except `;` `,` whitespace or control chars in
 * the unquoted form, or anything except `"` and control chars in
 * the quoted form. Quoted values: surrounding `"` is stripped.
 *
 * Returns a heap-allocated copy of the matched value, or NULL if
 * absent. Caller frees. */
static char* cookie_extract(const char* header_value, const char* name) {
    if (!header_value || !name) return NULL;
    size_t name_len = strlen(name);
    const char* p = header_value;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        const char* eq = strchr(p, '=');
        if (!eq) break;
        size_t key_len = (size_t)(eq - p);
        /* Trim trailing whitespace from key. */
        while (key_len > 0 && (p[key_len - 1] == ' ' || p[key_len - 1] == '\t')) key_len--;

        const char* val_start = eq + 1;
        const char* val_end = strchr(val_start, ';');
        if (!val_end) val_end = val_start + strlen(val_start);

        if (key_len == name_len && strncmp(p, name, name_len) == 0) {
            const char* vs = val_start;
            const char* ve = val_end;
            /* Strip surrounding quotes if present. */
            if (ve > vs && *vs == '"' && ve[-1] == '"') {
                vs++;
                ve--;
            }
            size_t vlen = (size_t)(ve - vs);
            char* out = (char*)malloc(vlen + 1);
            if (!out) return NULL;
            memcpy(out, vs, vlen);
            out[vlen] = '\0';
            return out;
        }

        p = (*val_end == ';') ? val_end + 1 : val_end;
    }
    return NULL;
}

int aether_middleware_session_auth(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    AetherSessionAuthOpts* o = (AetherSessionAuthOpts*)user_data;
    if (!o || !o->verify) return 1;

    const char* cookie_hdr = http_get_header(req, "Cookie");
    char* value = cookie_hdr ? cookie_extract(cookie_hdr, o->cookie_name) : NULL;

    int ok = (value != NULL) && o->verify(value, o->verifier_user_data);
    free(value);

    if (!ok) {
        if (o->redirect_url) {
            http_response_set_header(res, "Location", o->redirect_url);
            http_response_set_status(res, 302);
            http_response_set_body(res, "");
        } else {
            http_response_set_status(res, 401);
            http_response_set_body(res, "Unauthorized");
        }
        return 0;
    }
    return 1;
}

// -----------------------------------------------------------------
// Token-bucket rate limiter (per-client-IP)
// -----------------------------------------------------------------
typedef struct RateBucket {
    char* key;          /* X-Forwarded-For value or fallback */
    double tokens;      /* current bucket level */
    long   last_refill; /* monotonic ms */
    struct RateBucket* next;
} RateBucket;

#define RATE_TABLE_SIZE 256

struct AetherRateLimitOpts {
    int max_requests;
    int window_ms;
    pthread_mutex_t lock;
    RateBucket* table[RATE_TABLE_SIZE];
};

static long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static unsigned int rate_hash(const char* s) {
    unsigned int h = 5381;
    while (*s) { h = ((h << 5) + h) + (unsigned char)*s; s++; }
    return h;
}

AetherRateLimitOpts* aether_rate_limit_opts_new(int max_requests, int window_ms) {
    if (max_requests <= 0 || window_ms <= 0) return NULL;
    AetherRateLimitOpts* o = (AetherRateLimitOpts*)calloc(1, sizeof(AetherRateLimitOpts));
    if (!o) return NULL;
    o->max_requests = max_requests;
    o->window_ms = window_ms;
    pthread_mutex_init(&o->lock, NULL);
    return o;
}

void aether_rate_limit_opts_free(AetherRateLimitOpts* o) {
    if (!o) return;
    for (int i = 0; i < RATE_TABLE_SIZE; i++) {
        RateBucket* b = o->table[i];
        while (b) {
            RateBucket* next = b->next;
            free(b->key);
            free(b);
            b = next;
        }
    }
    pthread_mutex_destroy(&o->lock);
    free(o);
}

int aether_middleware_rate_limit(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    AetherRateLimitOpts* o = (AetherRateLimitOpts*)user_data;
    if (!o) return 1;

    /* Best-effort client identifier: prefer X-Forwarded-For (proxy
     * deployments), then X-Real-IP, then "anonymous". The HTTP
     * server doesn't currently expose the peer's sockaddr through
     * HttpRequest; this is an acceptable approximation for a
     * front-by-default token bucket. */
    const char* key = http_get_header(req, "X-Forwarded-For");
    if (!key) key = http_get_header(req, "X-Real-IP");
    if (!key) key = "anonymous";

    /* Trim to the first comma (X-Forwarded-For can be a chain). */
    char keybuf[128];
    size_t klen = 0;
    while (*key && *key != ',' && klen + 1 < sizeof(keybuf)) {
        if (*key != ' ' && *key != '\t') keybuf[klen++] = *key;
        key++;
    }
    keybuf[klen] = '\0';

    unsigned int slot = rate_hash(keybuf) % RATE_TABLE_SIZE;
    long now = monotonic_ms();

    pthread_mutex_lock(&o->lock);
    RateBucket* bucket = o->table[slot];
    RateBucket* prev = NULL;
    while (bucket && strcmp(bucket->key, keybuf) != 0) {
        prev = bucket;
        bucket = bucket->next;
    }
    if (!bucket) {
        bucket = (RateBucket*)calloc(1, sizeof(RateBucket));
        if (!bucket) {
            pthread_mutex_unlock(&o->lock);
            return 1;  /* fail open on OOM */
        }
        bucket->key = strdup(keybuf);
        if (!bucket->key) {
            // A NULL key would crash the strcmp bucket lookup on the next
            // request; drop the bucket and fail open like the calloc path above.
            free(bucket);
            pthread_mutex_unlock(&o->lock);
            return 1;  /* fail open on OOM */
        }
        bucket->tokens = (double)o->max_requests;
        bucket->last_refill = now;
        if (prev) prev->next = bucket;
        else o->table[slot] = bucket;
    }

    /* Continuous refill: rate = max_requests / window_ms tokens per ms. */
    long elapsed = now - bucket->last_refill;
    if (elapsed > 0) {
        double refill = (double)elapsed * (double)o->max_requests / (double)o->window_ms;
        bucket->tokens += refill;
        if (bucket->tokens > (double)o->max_requests) {
            bucket->tokens = (double)o->max_requests;
        }
        bucket->last_refill = now;
    }

    if (bucket->tokens < 1.0) {
        /* Out of tokens — compute Retry-After in seconds. */
        double need = 1.0 - bucket->tokens;
        double per_ms = (double)o->max_requests / (double)o->window_ms;
        int retry_sec = (int)(need / per_ms / 1000.0) + 1;
        pthread_mutex_unlock(&o->lock);

        char buf[32];
        snprintf(buf, sizeof(buf), "%d", retry_sec);
        http_response_set_header(res, "Retry-After", buf);
        http_response_set_status(res, 429);
        http_response_set_body(res, "Too Many Requests");
        return 0;
    }
    bucket->tokens -= 1.0;
    pthread_mutex_unlock(&o->lock);
    return 1;
}

// -----------------------------------------------------------------
// Virtual host
// -----------------------------------------------------------------
struct AetherVhostMap {
    char** hosts;
    int    count;
    int    cap;
    int    fallback_status;  /* 0 -> use 404 */
};

AetherVhostMap* aether_vhost_map_new(int fallback_status) {
    AetherVhostMap* m = (AetherVhostMap*)calloc(1, sizeof(AetherVhostMap));
    if (!m) return NULL;
    m->fallback_status = fallback_status > 0 ? fallback_status : 404;
    return m;
}

void aether_vhost_map_free(AetherVhostMap* m) {
    if (!m) return;
    for (int i = 0; i < m->count; i++) free(m->hosts[i]);
    free(m->hosts);
    free(m);
}

int aether_vhost_register_host(AetherVhostMap* m, const char* host) {
    if (!m || !host || !*host) return -1;
    if (m->count >= m->cap) {
        int new_cap = m->cap > 0 ? m->cap * 2 : 4;
        char** nh = (char**)realloc(m->hosts, new_cap * sizeof(char*));
        if (!nh) return -1;
        m->hosts = nh;
        m->cap = new_cap;
    }
    // Don't store a NULL on allocation failure and then report success: a NULL
    // entry would crash the strcmp in aether_middleware_vhost on the next
    // request. Leave the map unchanged and signal failure instead.
    char* dup = strdup(host);
    if (!dup) return -1;
    m->hosts[m->count++] = dup;
    return 0;
}

int aether_middleware_vhost(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    AetherVhostMap* m = (AetherVhostMap*)user_data;
    if (!m) return 1;
    const char* host_hdr = http_get_header(req, "Host");
    if (!host_hdr) host_hdr = "";

    /* Strip the optional :port suffix for the comparison. */
    char host[256];
    size_t hlen = 0;
    while (*host_hdr && *host_hdr != ':' && hlen + 1 < sizeof(host)) {
        host[hlen++] = (char)tolower((unsigned char)*host_hdr++);
    }
    host[hlen] = '\0';

    for (int i = 0; i < m->count; i++) {
        if (strcasecmp(m->hosts[i], host) == 0) return 1;  /* allowed */
    }

    http_response_set_status(res, m->fallback_status);
    http_response_set_body(res, "Unknown host");
    return 0;
}

// =================================================================
// D2 — gzip / static_files / rewrite / error_pages
// =================================================================

// -----------------------------------------------------------------
// gzip response transformer
// -----------------------------------------------------------------
#ifdef AETHER_HAS_ZLIB
#include <zlib.h>
#endif

struct AetherGzipOpts {
    int min_size;
    int level;
};

AetherGzipOpts* aether_gzip_opts_new(int min_size, int level) {
    AetherGzipOpts* o = (AetherGzipOpts*)calloc(1, sizeof(AetherGzipOpts));
    if (!o) return NULL;
    o->min_size = min_size > 0 ? min_size : 256;
    if (level < 1 || level > 9) level = 6;
    o->level = level;
    return o;
}

void aether_gzip_opts_free(AetherGzipOpts* o) { free(o); }

#ifdef AETHER_HAS_ZLIB
/* Compress `in_data` (in_len bytes) into a freshly allocated gzip
 * stream. Returns the buffer and writes its compressed length into
 * *out_len and the underlying allocation size into *out_cap. NULL on
 * failure. Uses windowBits=15+16 to produce gzip framing (header +
 * deflate + CRC + ISIZE).
 *
 * Cap-aware (#343): the buffer is allocated via aether_caps_malloc,
 * so the caller MUST free it with aether_caps_free(buf, *out_cap) —
 * passing the allocation size (not out_len) keeps the cap counter
 * balanced. */
static unsigned char* gzip_compress(const unsigned char* in_data, size_t in_len,
                                    int level, size_t* out_len, size_t* out_cap) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    /* 15+16: max window + gzip wrapper. -15 alone would be raw
     * deflate; +16 tells zlib to emit gzip framing. */
    if (deflateInit2(&strm, level, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return NULL;
    }
    uLong bound = deflateBound(&strm, (uLong)in_len);
    /* bound is derived from in_len which is the response body length
     * — caller-controlled but plugin-reachable, so the alloc is
     * gated by the cap. */
    size_t alloc_cap = bound > 0 ? (size_t)bound : 1;
    unsigned char* out = (unsigned char*)aether_caps_malloc(alloc_cap);
    if (!out) { deflateEnd(&strm); return NULL; }
    *out_cap = alloc_cap;
    strm.next_in = (Bytef*)in_data;
    strm.avail_in = (uInt)in_len;
    strm.next_out = out;
    strm.avail_out = (uInt)bound;
    int rc = deflate(&strm, Z_FINISH);
    if (rc != Z_STREAM_END) {
        deflateEnd(&strm);
        aether_caps_free(out, alloc_cap);
        return NULL;
    }
    *out_len = strm.total_out;
    deflateEnd(&strm);
    return out;
}
#endif

void aether_xform_gzip(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    AetherGzipOpts* o = (AetherGzipOpts*)user_data;
    if (!o || !res || !res->body || res->body_length < (size_t)o->min_size) return;
    /* Don't double-encode if the handler already set Content-Encoding. */
    for (int i = 0; i < res->header_count; i++) {
        if (res->header_keys[i] &&
            strcasecmp(res->header_keys[i], "Content-Encoding") == 0) {
            return;
        }
    }
    /* Client must accept gzip. Tolerant matching — Accept-Encoding can
     * carry multiple tokens with q-values; a substring search for
     * "gzip" is good enough for the common case. */
    const char* ae = http_get_header(req, "Accept-Encoding");
    if (!ae) return;
    int wants_gzip = 0;
    for (const char* p = ae; *p; p++) {
        if ((p == ae || !isalnum((unsigned char)p[-1])) &&
            strncasecmp(p, "gzip", 4) == 0) {
            wants_gzip = 1;
            break;
        }
    }
    if (!wants_gzip) return;

#ifdef AETHER_HAS_ZLIB
    size_t out_len = 0;
    size_t comp_cap = 0;
    unsigned char* compressed = gzip_compress((const unsigned char*)res->body,
                                              res->body_length, o->level,
                                              &out_len, &comp_cap);
    if (!compressed) return;  /* leave response untouched on compression failure */
    if (out_len >= res->body_length) {
        /* Compression didn't help (already-compressed body, e.g.
         * already-gzipped image). Discard and leave alone. Free the
         * caps-allocated buffer with its allocation size. */
        aether_caps_free(compressed, comp_cap);
        return;
    }
    /* Replace body in place. The response struct owns the body
     * string via its own free path (http_server_response_free),
     * which calls aether_caps_free(res->body, res->body_cap) — so
     * the replacement must go through the cap allocator and keep
     * res->body_cap in sync, otherwise the cap counter drifts and
     * the eventual free passes a wrong size. Free the old body the
     * same way the server does (caps-aware with the tracked cap;
     * heap-safe even when the body was a libc strdup from
     * http_response_set_body, where body_cap is 0).
     * Body is binary (deflate stream); the trailing NUL is
     * defensive only — the wire path uses res->body_length, not
     * strlen(res->body). */
    aether_caps_free(res->body, res->body_cap);
    size_t body_cap = out_len + 1;
    res->body = (char*)aether_caps_malloc(body_cap);
    if (!res->body) {
        aether_caps_free(compressed, comp_cap);
        res->body_length = 0;
        res->body_cap = 0;
        return;
    }
    memcpy(res->body, compressed, out_len);
    res->body[out_len] = '\0';
    res->body_length = out_len;
    res->body_cap = body_cap;
    aether_caps_free(compressed, comp_cap);

    /* Sync Content-Length to the post-compression size, replacing
     * the value the route handler set via http_response_set_body.
     * Without this, the wire response advertises the original
     * length and the client either truncates or errors out. */
    char clbuf[32];
    snprintf(clbuf, sizeof(clbuf), "%zu", out_len);
    http_response_set_header(res, "Content-Length",   clbuf);
    http_response_set_header(res, "Content-Encoding", "gzip");
    http_response_set_header(res, "Vary",             "Accept-Encoding");
#else
    (void)res;
#endif
}

// -----------------------------------------------------------------
// Static file serving
// -----------------------------------------------------------------
struct AetherStaticOpts {
    char* url_prefix;   /* e.g. "/assets" or "" */
    char* root;         /* e.g. "/var/www/static" */
    size_t prefix_len;
};

AetherStaticOpts* aether_static_opts_new(const char* url_prefix, const char* root) {
    if (!root || !*root) return NULL;
    AetherStaticOpts* o = (AetherStaticOpts*)calloc(1, sizeof(AetherStaticOpts));
    if (!o) return NULL;
    o->url_prefix = strdup(url_prefix ? url_prefix : "");
    o->root = strdup(root);
    // A NULL url_prefix would crash the strlen just below; a NULL root would
    // crash path resolution when serving. Bail cleanly instead.
    if (!o->url_prefix || !o->root) {
        free(o->url_prefix);
        free(o->root);
        free(o);
        return NULL;
    }
    o->prefix_len = strlen(o->url_prefix);
    return o;
}

void aether_static_opts_free(AetherStaticOpts* o) {
    if (!o) return;
    free(o->url_prefix);
    free(o->root);
    free(o);
}

extern const char* http_mime_type(const char* path);
extern void http_serve_file(HttpServerResponse* res, const char* filepath);

int aether_middleware_static(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    AetherStaticOpts* o = (AetherStaticOpts*)user_data;
    if (!o) return 1;
    const char* path = http_request_path(req);
    if (!path) return 1;

    /* Path must start with the configured prefix. Prefix "" matches
     * everything (mount the static directory at root). */
    if (o->prefix_len > 0 &&
        strncmp(path, o->url_prefix, o->prefix_len) != 0) {
        return 1;  /* not ours, continue chain */
    }
    const char* tail = path + o->prefix_len;
    if (*tail == '\0') tail = "/";

    /* Block path traversal — refuse any "/.." segment. */
    if (strstr(tail, "/..") || strstr(tail, "../") ||
        strcmp(tail, "..") == 0) {
        http_response_set_status(res, 403);
        http_response_set_body(res, "Forbidden");
        return 0;
    }

    /* Build the filesystem path: <root> + <tail>. */
    char fspath[1024];
    int n = snprintf(fspath, sizeof(fspath), "%s%s%s",
                     o->root,
                     (tail[0] == '/' ? "" : "/"),
                     tail);
    if (n <= 0 || n >= (int)sizeof(fspath)) {
        http_response_set_status(res, 414);
        http_response_set_body(res, "URI too long");
        return 0;
    }

    /* http_serve_file populates the response on success or sets a
     * 404 / 500 on failure. Either way, short-circuit the chain
     * once we've taken responsibility for the path. */
    http_serve_file(res, fspath);
    return 0;
}

// -----------------------------------------------------------------
// URL rewriting
// -----------------------------------------------------------------
typedef struct RewriteRule {
    char* from_prefix;
    char* to_prefix;
    size_t from_len;
} RewriteRule;

struct AetherRewriteOpts {
    RewriteRule* rules;
    int count;
    int cap;
};

AetherRewriteOpts* aether_rewrite_opts_new(void) {
    return (AetherRewriteOpts*)calloc(1, sizeof(AetherRewriteOpts));
}

void aether_rewrite_opts_free(AetherRewriteOpts* o) {
    if (!o) return;
    for (int i = 0; i < o->count; i++) {
        free(o->rules[i].from_prefix);
        free(o->rules[i].to_prefix);
    }
    free(o->rules);
    free(o);
}

int aether_rewrite_add_rule(AetherRewriteOpts* o,
                            const char* from_prefix,
                            const char* to_prefix) {
    if (!o || !from_prefix || !to_prefix) return -1;
    if (o->count >= o->cap) {
        int new_cap = o->cap > 0 ? o->cap * 2 : 4;
        RewriteRule* nr = (RewriteRule*)realloc(o->rules, new_cap * sizeof(RewriteRule));
        if (!nr) return -1;
        o->rules = nr;
        o->cap = new_cap;
    }
    // Allocate both strings up front; on failure store neither (a half-added
    // rule with a NULL prefix would crash the rewrite path) and report failure.
    char* fp = strdup(from_prefix);
    char* tp = strdup(to_prefix);
    if (!fp || !tp) { free(fp); free(tp); return -1; }
    o->rules[o->count].from_prefix = fp;
    o->rules[o->count].to_prefix = tp;
    o->rules[o->count].from_len = strlen(from_prefix);
    o->count++;
    return 0;
}

/* Mutate req->path in place via free + strdup. The HttpRequest
 * struct owns its path via http_request_free's free(req->path). */
int aether_middleware_rewrite(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    (void)res;
    AetherRewriteOpts* o = (AetherRewriteOpts*)user_data;
    if (!o || !req || !req->path) return 1;

    for (int i = 0; i < o->count; i++) {
        RewriteRule* r = &o->rules[i];
        if (strncmp(req->path, r->from_prefix, r->from_len) == 0) {
            const char* tail = req->path + r->from_len;
            size_t need = strlen(r->to_prefix) + strlen(tail) + 1;
            char* np = (char*)malloc(need);
            if (!np) return 1;  /* fail open on OOM */
            snprintf(np, need, "%s%s", r->to_prefix, tail);
            free(req->path);
            req->path = np;
            return 1;  /* first match wins; continue chain */
        }
    }
    return 1;
}

// -----------------------------------------------------------------
// Custom error pages
// -----------------------------------------------------------------
typedef struct ErrorPage {
    int    status;
    char*  body;
    char*  content_type;
} ErrorPage;

struct AetherErrorPagesOpts {
    ErrorPage* pages;
    int count;
    int cap;
};

AetherErrorPagesOpts* aether_error_pages_opts_new(void) {
    return (AetherErrorPagesOpts*)calloc(1, sizeof(AetherErrorPagesOpts));
}

void aether_error_pages_opts_free(AetherErrorPagesOpts* o) {
    if (!o) return;
    for (int i = 0; i < o->count; i++) {
        free(o->pages[i].body);
        free(o->pages[i].content_type);
    }
    free(o->pages);
    free(o);
}

int aether_error_pages_register(AetherErrorPagesOpts* o,
                                int status_code,
                                const char* body,
                                const char* content_type) {
    if (!o || status_code < 100 || status_code > 599 || !body) return -1;
    if (o->count >= o->cap) {
        int new_cap = o->cap > 0 ? o->cap * 2 : 4;
        ErrorPage* np = (ErrorPage*)realloc(o->pages, new_cap * sizeof(ErrorPage));
        if (!np) return -1;
        o->pages = np;
        o->cap = new_cap;
    }
    // Allocate both strings up front; on failure store neither (a NULL body or
    // content_type would crash when serving the custom error page) and report
    // failure rather than leaving a corrupt page entry.
    char* body_dup = strdup(body);
    char* ct_dup = (content_type && *content_type)
                       ? strdup(content_type) : strdup("text/html; charset=utf-8");
    if (!body_dup || !ct_dup) { free(body_dup); free(ct_dup); return -1; }
    o->pages[o->count].status = status_code;
    o->pages[o->count].body = body_dup;
    o->pages[o->count].content_type = ct_dup;
    o->count++;
    return 0;
}

void aether_xform_error_pages(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    (void)req;
    AetherErrorPagesOpts* o = (AetherErrorPagesOpts*)user_data;
    if (!o || !res) return;
    for (int i = 0; i < o->count; i++) {
        if (o->pages[i].status == res->status_code) {
            http_response_set_body(res, o->pages[i].body);
            http_response_set_header(res, "Content-Type", o->pages[i].content_type);
            return;
        }
    }
}

// -----------------------------------------------------------------
// Real-IP / X-Forwarded-For
// -----------------------------------------------------------------
struct AetherRealIpOpts {
    char* header_name;  /* e.g. "X-Forwarded-For" */
};

AetherRealIpOpts* aether_real_ip_opts_new(const char* header_name) {
    AetherRealIpOpts* o = (AetherRealIpOpts*)calloc(1, sizeof(AetherRealIpOpts));
    if (!o) return NULL;
    if (!header_name || !*header_name) header_name = "X-Forwarded-For";
    o->header_name = strdup(header_name);
    if (!o->header_name) { free(o); return NULL; }
    return o;
}

void aether_real_ip_opts_free(AetherRealIpOpts* o) {
    if (!o) return;
    free(o->header_name);
    free(o);
}

/* Append a single header to req->header_keys/values. Mirrors the
 * memory ownership the HTTP/1.1 parser uses (each entry is a
 * malloc'd string freed by http_request_free). Returns 0 on
 * success, -1 on OOM. */
static int request_append_header(HttpRequest* req,
                                 const char* name,
                                 const char* value) {
    if (!req || !name || !value) return -1;
    int n = req->header_count;
    char** nk = (char**)realloc(req->header_keys,   (size_t)(n + 1) * sizeof(char*));
    if (!nk) return -1;
    req->header_keys = nk;
    char** nv = (char**)realloc(req->header_values, (size_t)(n + 1) * sizeof(char*));
    if (!nv) return -1;
    req->header_values = nv;
    char* kd = strdup(name);
    char* vd = strdup(value);
    // Store only if both copies succeed; returning with just one stored (the
    // old code) leaked it, since header_count wasn't advanced to cover index n.
    if (!kd || !vd) { free(kd); free(vd); return -1; }
    req->header_keys[n]   = kd;
    req->header_values[n] = vd;
    req->header_count = n + 1;
    return 0;
}

int aether_middleware_real_ip(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    (void)res;
    AetherRealIpOpts* o = (AetherRealIpOpts*)user_data;
    if (!o || !req) return 1;

    /* Find the configured forward-list header (case-insensitive). */
    const char* fwd = NULL;
    for (int i = 0; i < req->header_count; i++) {
        if (req->header_keys[i] &&
            strcasecmp(req->header_keys[i], o->header_name) == 0) {
            fwd = req->header_values[i];
            break;
        }
    }
    if (!fwd || !*fwd) {
        /* No forward chain — nothing to extract. Continue without
         * mutating the request; downstream handlers fall back to
         * whatever IP-source they already used. */
        return 1;
    }

    /* Take the leftmost token, trimmed of surrounding whitespace.
     * Per RFC 7239 / X-Forwarded-For convention, the leftmost
     * IP is the original client; subsequent entries are
     * intermediate proxies. */
    const char* p = fwd;
    while (*p == ' ' || *p == '\t') p++;
    const char* end = p;
    while (*end && *end != ',' && *end != ' ' && *end != '\t') end++;
    if (end == p) return 1;  /* empty token — give up */

    char ip_buf[64];
    size_t len = (size_t)(end - p);
    if (len >= sizeof(ip_buf)) len = sizeof(ip_buf) - 1;
    memcpy(ip_buf, p, len);
    ip_buf[len] = '\0';

    /* Idempotent: if X-Real-IP already set (we ran twice), don't
     * append a duplicate that would mask the first. */
    for (int i = 0; i < req->header_count; i++) {
        if (req->header_keys[i] &&
            strcasecmp(req->header_keys[i], "X-Real-IP") == 0) {
            return 1;
        }
    }
    request_append_header(req, "X-Real-IP", ip_buf);
    return 1;  /* always continue the chain — middleware is
                * informational, never short-circuits */
}

