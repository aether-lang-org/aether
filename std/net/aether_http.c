#include "aether_http.h"
#include "../../runtime/config/aether_optimization_config.h"
#include "../../runtime/aether_resource_caps.h"

#if !AETHER_HAS_NETWORKING
HttpResponse* http_get_raw(const char* u) { (void)u; return NULL; }
HttpResponse* http_get_with_timeout_ns_raw(const char* u, int64_t ns) { (void)u; (void)ns; return NULL; }
HttpResponse* http_post_raw(const char* u, const char* b, const char* c) { (void)u; (void)b; (void)c; return NULL; }
HttpResponse* http_put_raw(const char* u, const char* b, const char* c) { (void)u; (void)b; (void)c; return NULL; }
HttpResponse* http_delete_raw(const char* u) { (void)u; return NULL; }
void http_response_free(HttpResponse* r) { (void)r; }
int http_response_status(HttpResponse* r) { (void)r; return 0; }
const char* http_response_body(HttpResponse* r) { (void)r; return ""; }
int http_response_body_length(HttpResponse* r) { (void)r; return 0; }
const char* http_response_headers(HttpResponse* r) { (void)r; return ""; }
const char* http_response_error(HttpResponse* r) { (void)r; return "networking disabled at build time"; }
int http_response_ok(HttpResponse* r) { (void)r; return 0; }
struct HttpRequest { int unused; };
HttpRequest* http_request_raw(const char* m, const char* u) { (void)m; (void)u; return NULL; }
int http_request_set_header_raw(HttpRequest* r, const char* n, const char* v) { (void)r; (void)n; (void)v; return -1; }
int http_request_set_body_raw(HttpRequest* r, const char* b, int l, const char* c) { (void)r; (void)b; (void)l; (void)c; return -1; }
int http_request_set_timeout_raw(HttpRequest* r, int s) { (void)r; (void)s; return -1; }
int http_request_set_timeout_ns_raw(HttpRequest* r, int64_t ns) { (void)r; (void)ns; return -1; }
int http_request_set_follow_redirects_raw(HttpRequest* r, int n) { (void)r; (void)n; return -1; }
void http_request_free_raw(HttpRequest* r) { (void)r; }
HttpResponse* http_send_raw(HttpRequest* r) { (void)r; return NULL; }
const char* http_response_header_raw(HttpResponse* r, const char* n) { (void)r; (void)n; return ""; }
const char* http_response_effective_url_raw(HttpResponse* r) { (void)r; return ""; }
const char* http_response_redirect_error_raw(HttpResponse* r) { (void)r; return ""; }
#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <limits.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib")
    #endif
    #define close closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <arpa/inet.h>
    #include <fcntl.h>       /* fcntl, O_NONBLOCK for connect-with-timeout */
    #include <errno.h>       /* EINPROGRESS / EWOULDBLOCK detection      */
    #include <sys/select.h>  /* select(), fd_set                          */
    #include <sys/time.h>    /* struct timeval                            */
#endif

#ifdef AETHER_HAS_OPENSSL
    #include <openssl/ssl.h>
    #include <openssl/err.h>
    #include <openssl/x509v3.h>
#endif

static int http_initialized = 0;

static void http_init(void) {
    if (http_initialized) return;
    #ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
    #endif
    http_initialized = 1;
}

// -----------------------------------------------------------------
// URL parsing
// -----------------------------------------------------------------

// Parse `url` into (host, port, path, use_tls).
//   https://foo.example/bar  →  host="foo.example" port=443 path="/bar" use_tls=1
//   http://foo:8080/         →  host="foo"         port=8080 path="/"   use_tls=0
// Returns 1 on success, 0 on malformed input.
static int parse_url(const char* url, char* host, size_t host_size,
                     int* port, char* path, size_t path_size, int* use_tls) {
    if (!url || !host || !port || !path || !use_tls ||
        host_size == 0 || path_size == 0) return 0;

    const char* start;
    *use_tls = 0;

    if (strncmp(url, "http://", 7) == 0) {
        start = url + 7;
        *port = 80;
    } else if (strncmp(url, "https://", 8) == 0) {
        start = url + 8;
        *port = 443;
        *use_tls = 1;
    } else {
        start = url;
        *port = 80;
    }

    const char* slash = strchr(start, '/');
    const char* colon = strchr(start, ':');

    if (colon && (!slash || colon < slash)) {
        size_t host_len = colon - start;
        if (host_len >= host_size) host_len = host_size - 1;
        memcpy(host, start, host_len);
        host[host_len] = '\0';
        *port = atoi(colon + 1);
        if (slash) {
            snprintf(path, path_size, "%s", slash);
        } else {
            snprintf(path, path_size, "/");
        }
    } else if (slash) {
        size_t host_len = slash - start;
        if (host_len >= host_size) host_len = host_size - 1;
        memcpy(host, start, host_len);
        host[host_len] = '\0';
        snprintf(path, path_size, "%s", slash);
    } else {
        snprintf(host, host_size, "%s", start);
        snprintf(path, path_size, "/");
    }

    return 1;
}

// -----------------------------------------------------------------
// OpenSSL context: lazy init, shared across all HTTPS calls
// -----------------------------------------------------------------

#ifdef AETHER_HAS_OPENSSL

static _Atomic(SSL_CTX*) g_ssl_ctx;

#ifdef _WIN32
// Probe well-known CA-bundle locations on Windows in priority order and
// load the first one that exists. Linux + macOS have a system trust
// store that OpenSSL's compiled-in default paths point at; Windows
// MSYS2 builds keep theirs at MINGW_PREFIX/etc/ssl/certs/ca-bundle.crt
// (from the `mingw-w64-x86_64-ca-certificates` package), and the Aether
// release archive includes a bundle next to ae.exe at
// <root>/share/ssl/ca-bundle.crt. Without a trust anchor every HTTPS
// request fails with "certificate verify failed".
//
// Returns 1 if a bundle was successfully loaded, 0 if no probe matched.
static int load_windows_ca_bundle(SSL_CTX* ctx) {
    // 1. SSL_CERT_FILE env var — also honored by SSL_CTX_set_default_
    //    verify_paths, but try it first so the load succeeds even when
    //    the default paths weren't compiled in.
    const char* env = getenv("SSL_CERT_FILE");
    if (env && env[0] && SSL_CTX_load_verify_locations(ctx, env, NULL) == 1) {
        return 1;
    }

    // 2. Bundle shipped alongside ae.exe in the release archive
    //    (<root>/bin/ae.exe → <root>/share/ssl/ca-bundle.crt).
    char exe_path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    if (n > 0 && n < sizeof(exe_path)) {
        char* p = strrchr(exe_path, '\\');           // …\bin\ae.exe → …\bin
        if (p) {
            *p = '\0';
            char* q = strrchr(exe_path, '\\');        // …\bin → …
            if (q && _stricmp(q, "\\bin") == 0) *q = '\0';
        }
        char bundle[MAX_PATH + 64];
        snprintf(bundle, sizeof(bundle),
                 "%s\\share\\ssl\\ca-bundle.crt", exe_path);
        if (SSL_CTX_load_verify_locations(ctx, bundle, NULL) == 1) {
            return 1;
        }
    }

    // 3. MSYS2 well-known paths (mingw-w64-x86_64-ca-certificates package).
    static const char* candidates[] = {
        "C:\\msys64\\mingw64\\etc\\ssl\\certs\\ca-bundle.crt",
        "C:\\msys64\\ucrt64\\etc\\ssl\\certs\\ca-bundle.crt",
        "C:\\msys64\\mingw64\\etc\\ssl\\cert.pem",
        "C:\\msys64\\ucrt64\\etc\\ssl\\cert.pem",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (SSL_CTX_load_verify_locations(ctx, candidates[i], NULL) == 1) {
            return 1;
        }
    }
    return 0;
}
#endif  // _WIN32

// Get (or lazily create) the shared SSL_CTX. Benign race on first call —
// compare-exchange ensures at most one SSL_CTX is installed even if two
// threads reach here simultaneously. Returns NULL on OpenSSL error.
static SSL_CTX* get_ssl_ctx(void) {
    SSL_CTX* ctx = atomic_load(&g_ssl_ctx);
    if (ctx) return ctx;

    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

#ifdef _WIN32
    // Windows OpenSSL builds typically have no compiled-in CA path — probe
    // SSL_CERT_FILE / the shipped bundle / MSYS2 paths first; only fall
    // through to the default-paths call (which is effectively a no-op on
    // Win32) if every probe missed.
    if (!load_windows_ca_bundle(ctx)) {
        SSL_CTX_set_default_verify_paths(ctx);
    }
#else
    // Linux/macOS: the system trust store lives at the path OpenSSL was
    // compiled with — /etc/ssl/certs and the macOS keychain bridge.
    SSL_CTX_set_default_verify_paths(ctx);
#endif

    SSL_CTX* expected = NULL;
    if (!atomic_compare_exchange_strong(&g_ssl_ctx, &expected, ctx)) {
        SSL_CTX_free(ctx);
        ctx = expected;
    }
    return ctx;
}

// Flush OpenSSL's error queue into a newly allocated string. Used only
// on error paths to surface the underlying OpenSSL reason.
static char* ssl_err_string(const char* prefix) {
    unsigned long err = ERR_get_error();
    const char* detail = err ? ERR_reason_error_string(err) : NULL;
    size_t plen = strlen(prefix);
    size_t dlen = detail ? strlen(detail) : 0;
    char* msg = (char*)malloc(plen + (dlen ? dlen + 3 : 0) + 1);
    if (!msg) return NULL;
    memcpy(msg, prefix, plen);
    if (dlen) {
        memcpy(msg + plen, ": ", 2);
        memcpy(msg + plen + 2, detail, dlen);
        msg[plen + 2 + dlen] = '\0';
    } else {
        msg[plen] = '\0';
    }
    return msg;
}

#endif // AETHER_HAS_OPENSSL

// -----------------------------------------------------------------
// Transport abstraction
//
// `Transport` wraps either a raw socket or an SSL* connection so the
// request/response loop doesn't need to branch on protocol everywhere.
// send/recv callbacks match the BSD socket signatures so plaintext
// paths can use them as-is.
// -----------------------------------------------------------------

typedef struct {
    int sockfd;
#ifdef AETHER_HAS_OPENSSL
    SSL* ssl;
#endif
} Transport;

static int transport_send(Transport* t, const void* buf, int len) {
#ifdef AETHER_HAS_OPENSSL
    if (t->ssl) return SSL_write(t->ssl, buf, len);
#endif
    return (int)send(t->sockfd, buf, len, 0);
}

static int transport_recv(Transport* t, void* buf, int len) {
#ifdef AETHER_HAS_OPENSSL
    if (t->ssl) return SSL_read(t->ssl, buf, len);
#endif
    return (int)recv(t->sockfd, buf, len, 0);
}

static void transport_close(Transport* t) {
#ifdef AETHER_HAS_OPENSSL
    if (t->ssl) {
        SSL_shutdown(t->ssl);
        SSL_free(t->ssl);
        t->ssl = NULL;
    }
#endif
    if (t->sockfd >= 0) {
        close(t->sockfd);
        t->sockfd = -1;
    }
}

// -----------------------------------------------------------------
// v2 request builder — opaque struct + per-field setters. The v1
// one-liners (http_get_raw / http_post_raw / etc.) build a request
// internally and call http_send_raw, so all paths funnel through
// the same socket / TLS code below.
// -----------------------------------------------------------------

typedef struct HttpHeader {
    char* name;
    char* value;
    struct HttpHeader* next;
} HttpHeader;

struct HttpRequest {
    char* method;        /* "GET", "POST", etc. — owned, NUL-terminated */
    char* url;           /* full URL — owned, NUL-terminated */
    HttpHeader* headers; /* singly-linked, in insertion order */
    char* body;          /* may be NULL; binary-safe via body_len */
    int   body_len;      /* explicit length; 0 if no body */
    char* content_type;  /* may be NULL; defaults applied at send time */
    int64_t timeout_ns;  /* 0 = no timeout (block forever). Whole-ns
                          * precision; the platform timeout calls
                          * (select tv, SO_RCVTIMEO timeval, Winsock
                          * DWORD ms) slice this down to whatever
                          * granularity they support. */
    int   max_redirects; /* 0 = don't follow (default); N>0 = follow up to N hops */
};

HttpRequest* http_request_raw(const char* method, const char* url) {
    if (!method || !*method || !url || !*url) return NULL;
    HttpRequest* req = (HttpRequest*)calloc(1, sizeof(HttpRequest));
    if (!req) return NULL;
    req->method = strdup(method);
    req->url    = strdup(url);
    if (!req->method || !req->url) {
        http_request_free_raw(req);
        return NULL;
    }
    return req;
}

int http_request_set_header_raw(HttpRequest* req, const char* name, const char* value) {
    if (!req || !name || !*name || !value) return -1;
    HttpHeader* h = (HttpHeader*)calloc(1, sizeof(HttpHeader));
    if (!h) return -1;
    h->name  = strdup(name);
    h->value = strdup(value);
    if (!h->name || !h->value) {
        free(h->name); free(h->value); free(h);
        return -1;
    }
    /* Append at the tail so emission order matches insertion order;
     * keeps tests deterministic and avoids surprises with servers
     * that care about header ordering (rare but exists). */
    if (!req->headers) {
        req->headers = h;
    } else {
        HttpHeader* tail = req->headers;
        while (tail->next) tail = tail->next;
        tail->next = h;
    }
    return 0;
}

int http_request_set_body_raw(HttpRequest* req, const char* body, int len, const char* content_type) {
    if (!req || len < 0) return -1;
    /* Replace any prior body. Cap-aware (#343): req->body_len holds
     * the original allocation size and is reset to 0 below before
     * the next aether_caps_malloc. */
    aether_caps_free(req->body, (size_t)req->body_len);
    req->body = NULL; req->body_len = 0;
    free(req->content_type); req->content_type = NULL;
    if (len > 0) {
        if (!body) return -1;
        /* `body` may be either an AetherString* (when the caller
         * passed an Aether string variable — common for binary
         * payloads from fs.read_binary) or a plain char* (string
         * literals). Without this unwrap, a 10-byte AetherString
         * input would copy the 24-byte struct header (magic +
         * refcount + length + capacity + data-ptr) into our body
         * buffer, and the wire would carry that header. Same shape
         * the std.fs / std.cryptography / std.zlib externs use. */
        const char* src = body;
        if (is_aether_string(body)) {
            src = ((const AetherString*)body)->data;
        }
        /* Cap-aware (#343): caller-supplied length, untrusted on
         * --emit=lib paths. The matching aether_caps_free passes
         * req->body_len as the size. */
        req->body = (char*)aether_caps_malloc((size_t)len);
        if (!req->body) return -1;
        memcpy(req->body, src, (size_t)len);
        req->body_len = len;
    }
    if (content_type) {
        req->content_type = strdup(content_type);
        if (!req->content_type) return -1;
    }
    return 0;
}

int http_request_set_timeout_raw(HttpRequest* req, int seconds) {
    if (!req || seconds < 0) return -1;
    req->timeout_ns = (int64_t)seconds * 1000000000LL;
    return 0;
}

int http_request_set_timeout_ns_raw(HttpRequest* req, int64_t timeout_ns) {
    if (!req || timeout_ns < 0) return -1;
    req->timeout_ns = timeout_ns;
    return 0;
}

int http_request_set_follow_redirects_raw(HttpRequest* req, int max_hops) {
    if (!req || max_hops < 0) return -1;
    req->max_redirects = max_hops;
    return 0;
}

void http_request_free_raw(HttpRequest* req) {
    if (!req) return;
    free(req->method);
    free(req->url);
    /* Body was alloc'd via aether_caps_malloc in
     * http_request_set_body_raw; pair the free with the recorded
     * length so cap accounting stays at current-usage. */
    aether_caps_free(req->body, (size_t)req->body_len);
    free(req->content_type);
    HttpHeader* h = req->headers;
    while (h) {
        HttpHeader* next = h->next;
        free(h->name); free(h->value); free(h);
        h = next;
    }
    free(req);
}

/* Forward decls — defined below the static request() function. */
static int header_already_set(HttpRequest* req, const char* name);
static char* http_extract_response_header(const char* hdr_block, const char* name);
/* Decode HTTP/1.1 chunked transfer-encoding. Returns a malloc'd decoded
 * buffer (NUL-terminated; *out_len excludes the NUL) or NULL on malformed
 * framing (caller then keeps the raw body). Binary-safe (copies by
 * length). Defined below. */
static char* http_dechunk(const char* in, size_t in_len, size_t* out_len);
static int http_value_has_chunked(const char* v);

// -----------------------------------------------------------------
// Core request — operates on an HttpRequest. v1 wrappers build a
// throwaway HttpRequest and discard it after send.
// -----------------------------------------------------------------

static HttpResponse* http_request_internal(HttpRequest* req) {
    const char* method = req->method;
    const char* url    = req->url;
    const char* body   = req->body;          /* may be NULL */
    int   body_len     = req->body_len;
    const char* content_type = req->content_type;
    http_init();

    HttpResponse* response = (HttpResponse*)malloc(sizeof(HttpResponse));
    if (!response) return NULL;
    response->status_code = 0;
    response->body = NULL;
    response->headers = NULL;
    response->error = NULL;
    response->redirect_error = NULL;
    response->effective_url = NULL;

    char host[256];
    char path[1024];
    int port;
    int use_tls;

    if (!parse_url(url, host, sizeof(host), &port, path, sizeof(path), &use_tls)) {
        response->error = string_new("malformed URL");
        return response;
    }

    if (use_tls) {
#ifndef AETHER_HAS_OPENSSL
        response->error = string_new("HTTPS requested but the build has no OpenSSL support (rebuild with OpenSSL installed)");
        return response;
#endif
    }

    struct hostent* server = gethostbyname(host);
    if (!server) {
        response->error = string_new("could not resolve host");
        return response;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        response->error = string_new("could not create socket");
        return response;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(port);

    /* Connect — with timeout via non-blocking + select when the
     * caller asked for one. Without a timeout, fall through to the
     * original blocking connect (preserves v1 behaviour exactly). */
    int connect_rc;
    if (req->timeout_ns > 0) {
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(sockfd, FIONBIO, &nb);
#else
        int flags = fcntl(sockfd, F_GETFL, 0);
        if (flags >= 0) fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
#endif
        connect_rc = connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
        if (connect_rc < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            int in_progress = (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
            int in_progress = (errno == EINPROGRESS || errno == EWOULDBLOCK);
#endif
            if (in_progress) {
                /* Watch both the writable set (success) and the
                 * exception set (failure). On POSIX, a refused
                 * non-blocking connect makes the socket writable
                 * with SO_ERROR=ECONNREFUSED — only `wfds` matters.
                 * On Windows, Winsock signals connect failures via
                 * the *exception* fd set instead, NOT writable —
                 * so a select that watches only wfds waits the full
                 * timeout for refused connects rather than failing
                 * fast. Watching both makes select fire on either
                 * outcome; SO_ERROR distinguishes success from
                 * failure afterwards. */
                fd_set wfds, efds;
                FD_ZERO(&wfds); FD_SET(sockfd, &wfds);
                FD_ZERO(&efds); FD_SET(sockfd, &efds);
                /* `select` takes microsecond precision via tv_usec —
                 * slice the configured nanoseconds straight in.
                 * tv_usec is `suseconds_t` on POSIX and `long` on
                 * MinGW; cast to `long` to satisfy both portably. */
                struct timeval tv;
                tv.tv_sec  = (time_t)(req->timeout_ns / 1000000000LL);
                tv.tv_usec = (long)((req->timeout_ns / 1000LL) % 1000000LL);
                int sel = select(sockfd + 1, NULL, &wfds, &efds, &tv);
                if (sel == 0) {
                    close(sockfd);
                    response->error = string_new("connect timeout");
                    return response;
                }
                if (sel < 0) {
                    close(sockfd);
                    response->error = string_new("select on connect failed");
                    return response;
                }
                /* Check SO_ERROR — distinguishes "writable because
                 * connected" from "writable because failed". On
                 * Windows the failure surfaces via efds; on POSIX
                 * via wfds with so_err set. SO_ERROR works the
                 * same in both cases. */
                int so_err = 0;
                socklen_t slen = sizeof(so_err);
                if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (char*)&so_err, &slen) < 0
                    || so_err != 0) {
                    close(sockfd);
                    response->error = string_new("connection failed");
                    return response;
                }
                connect_rc = 0;
            } else {
                close(sockfd);
                response->error = string_new("connection failed");
                return response;
            }
        }
        /* Restore blocking mode for the send/recv path — those use
         * setsockopt(SO_*TIMEO) below for their timeouts. */
#ifdef _WIN32
        nb = 0;
        ioctlsocket(sockfd, FIONBIO, &nb);
#else
        if (flags >= 0) fcntl(sockfd, F_SETFL, flags);
#endif

        /* Apply send/recv timeouts equal to the configured value.
         *
         * SO_RCVTIMEO / SO_SNDTIMEO take different shapes on the two
         * families:
         *   - POSIX: pointer to `struct timeval` (seconds + microseconds)
         *   - Winsock: pointer to a 32-bit DWORD in milliseconds
         *
         * Passing a struct timeval to Winsock causes it to interpret
         * the first 4 bytes (tv_sec) as a millisecond count — so
         * `set_timeout(35)` would degrade to a 35-millisecond recv
         * timeout, which fires almost instantly and surfaces as
         * "recv timeout or I/O error" before any sane upstream can
         * even respond. Use the right type per platform.
         *
         * Both shapes carry sub-second precision: POSIX gets the full
         * microsecond resolution via tv_usec; Winsock rounds up to
         * the next whole millisecond (sub-ms DWORD=0 is ambiguous
         * with "infinite"). */
#ifdef _WIN32
        /* (ns + 999999) / 1000000 = ms rounded up; 1ns becomes 1ms. */
        int64_t ms_total = (req->timeout_ns + 999999LL) / 1000000LL;
        if (ms_total > 0xFFFFFFFFLL) ms_total = 0xFFFFFFFFLL;
        DWORD rwtv_ms = (DWORD)ms_total;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rwtv_ms, sizeof(rwtv_ms));
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&rwtv_ms, sizeof(rwtv_ms));
#else
        struct timeval rwtv;
        rwtv.tv_sec  = (time_t)(req->timeout_ns / 1000000000LL);
        rwtv.tv_usec = (long)((req->timeout_ns / 1000LL) % 1000000LL);
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rwtv, sizeof(rwtv));
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&rwtv, sizeof(rwtv));
#endif
    } else {
        connect_rc = connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
        if (connect_rc < 0) {
            close(sockfd);
            response->error = string_new("connection failed");
            return response;
        }
    }

    Transport t;
    t.sockfd = sockfd;
#ifdef AETHER_HAS_OPENSSL
    t.ssl = NULL;

    if (use_tls) {
        SSL_CTX* ctx = get_ssl_ctx();
        if (!ctx) {
            close(sockfd);
            char* msg = ssl_err_string("TLS context init failed");
            response->error = string_new(msg ? msg : "TLS context init failed");
            free(msg);
            return response;
        }

        SSL* ssl = SSL_new(ctx);
        if (!ssl) {
            close(sockfd);
            char* msg = ssl_err_string("SSL_new failed");
            response->error = string_new(msg ? msg : "SSL_new failed");
            free(msg);
            return response;
        }

        // SNI: server-name indication so virtual-hosted TLS services
        // return the right cert.
        SSL_set_tlsext_host_name(ssl, host);

        // Verify the cert's CN/SAN matches the hostname we connected to.
        X509_VERIFY_PARAM* vpm = SSL_get0_param(ssl);
        X509_VERIFY_PARAM_set_hostflags(vpm, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        X509_VERIFY_PARAM_set1_host(vpm, host, 0);

        SSL_set_fd(ssl, sockfd);
        int connect_result = SSL_connect(ssl);
        if (connect_result != 1) {
            int ssl_err = SSL_get_error(ssl, connect_result);
            (void)ssl_err;
            SSL_free(ssl);
            close(sockfd);
            char* msg = ssl_err_string("TLS handshake failed");
            response->error = string_new(msg ? msg : "TLS handshake failed");
            free(msg);
            return response;
        }

        t.ssl = ssl;
    }
#endif

    /* Build the header block in a heap-allocated growing buffer so
     * we're not bounded by a 4K stack array. Body goes out as a
     * separate transport_send so binary payloads with embedded NULs
     * survive (the previous "%s" snprintf path would have truncated
     * at the first NUL — wasn't a problem in practice because the
     * v1 wrappers only sent textual JSON, but v2 takes body+len). */
    size_t hdr_cap = 1024;
    char* hdr = (char*)malloc(hdr_cap);
    if (!hdr) {
        transport_close(&t);
        response->error = string_new("out of memory building request");
        return response;
    }
    size_t hdr_len = 0;

    /* Helper: append a NUL-terminated string into hdr, growing as
     * needed. Returns 0 on success, -1 on OOM. */
    #define HDR_APPEND_STR(s) do { \
        size_t _slen = strlen(s); \
        if (hdr_len + _slen + 1 > hdr_cap) { \
            size_t _nc = hdr_cap; \
            while (_nc < hdr_len + _slen + 1) _nc *= 2; \
            char* _nh = (char*)realloc(hdr, _nc); \
            if (!_nh) { free(hdr); transport_close(&t); \
                       response->error = string_new("out of memory building request"); \
                       return response; } \
            hdr = _nh; hdr_cap = _nc; \
        } \
        memcpy(hdr + hdr_len, s, _slen); \
        hdr_len += _slen; \
        hdr[hdr_len] = '\0'; \
    } while (0)

    /* Request line. */
    HDR_APPEND_STR(method); HDR_APPEND_STR(" "); HDR_APPEND_STR(path); HDR_APPEND_STR(" HTTP/1.1\r\n");

    /* Built-in Host (overridable via set_header). */
    if (!header_already_set(req, "Host")) {
        HDR_APPEND_STR("Host: "); HDR_APPEND_STR(host); HDR_APPEND_STR("\r\n");
    }

    /* Built-in Content-Length when body present (overridable, but
     * setting it manually is almost always a bug — we still emit
     * ours unless the caller explicitly overrode it). */
    if (body && body_len > 0 && !header_already_set(req, "Content-Length")) {
        char clen[32];
        snprintf(clen, sizeof(clen), "Content-Length: %d\r\n", body_len);
        HDR_APPEND_STR(clen);
    }

    /* Built-in Content-Type when body present, only if neither the
     * builder's content_type nor an explicit Content-Type header is set. */
    if (body && body_len > 0 && content_type
        && !header_already_set(req, "Content-Type")) {
        HDR_APPEND_STR("Content-Type: "); HDR_APPEND_STR(content_type); HDR_APPEND_STR("\r\n");
    } else if (body && body_len > 0 && !content_type
        && !header_already_set(req, "Content-Type")) {
        HDR_APPEND_STR("Content-Type: application/x-www-form-urlencoded\r\n");
    }

    /* Connection: close (overridable — keep-alive is out of scope
     * for v2 but a caller is welcome to ask for it). */
    if (!header_already_set(req, "Connection")) {
        HDR_APPEND_STR("Connection: close\r\n");
    }

    /* Caller-provided headers, in insertion order. */
    for (HttpHeader* h = req->headers; h; h = h->next) {
        HDR_APPEND_STR(h->name); HDR_APPEND_STR(": "); HDR_APPEND_STR(h->value); HDR_APPEND_STR("\r\n");
    }

    /* End-of-headers blank line. */
    HDR_APPEND_STR("\r\n");

    #undef HDR_APPEND_STR

    if (transport_send(&t, hdr, (int)hdr_len) < 0) {
        free(hdr);
        transport_close(&t);
        response->error = string_new("send failed");
        return response;
    }
    free(hdr);

    /* Body — emitted raw so embedded NULs survive. */
    if (body && body_len > 0) {
        if (transport_send(&t, body, body_len) < 0) {
            transport_close(&t);
            response->error = string_new("send failed");
            return response;
        }
    }

    // Accumulator grows with capacity doubling. The previous
    // realloc-per-recv pattern was quadratic on large responses;
    // doubling amortises growth to O(n).
    char   buffer[8192];
    char*  full_response = NULL;
    size_t total_len = 0;
    size_t cap = 0;
    int    n;

    int recv_err = 0;  /* set if the loop exits via timeout / I/O error */
    while ((n = transport_recv(&t, buffer, sizeof(buffer) - 1)) > 0) {
        if (total_len + (size_t)n + 1 > cap) {
            size_t new_cap = cap ? cap * 2 : 16384;
            while (new_cap < total_len + (size_t)n + 1) new_cap *= 2;
            char* new_resp = (char*)realloc(full_response, new_cap);
            if (!new_resp) {
                free(full_response);
                transport_close(&t);
                response->error = string_new("out of memory reading response");
                return response;
            }
            full_response = new_resp;
            cap = new_cap;
        }
        memcpy(full_response + total_len, buffer, (size_t)n);
        total_len += (size_t)n;
        full_response[total_len] = '\0';
    }
    /* n < 0 means transport_recv hit an error; the most common one
     * (when the caller set a timeout) is recv-side EAGAIN/EWOULDBLOCK
     * from SO_RCVTIMEO firing. Without this, a timed-out request
     * silently returned an empty 0-status response — caller couldn't
     * tell timeout from a happy server returning nothing. */
    if (n < 0) {
        recv_err = 1;
    }

    // Zero-byte response: still need a valid empty string so strstr
    // below is safe. Tiny allocation, done only on the empty path.
    if (!full_response) {
        full_response = (char*)malloc(1);
        if (!full_response) {
            transport_close(&t);
            response->error = string_new("out of memory");
            return response;
        }
        full_response[0] = '\0';
    }

    transport_close(&t);

    /* If transport_recv reported an error AND we didn't get a complete
     * header block, this was a timeout / I/O error mid-recv. Tell the
     * caller — otherwise they'd see status=0 + empty body and not
     * know whether the request even reached the server. */
    char* header_end = strstr(full_response, "\r\n\r\n");
    if (recv_err && !header_end) {
        free(full_response);
        response->error = string_new("recv timeout or I/O error");
        return response;
    }
    if (header_end) {
        size_t header_bytes = (size_t)((header_end + 4) - full_response);
        size_t body_bytes = total_len >= header_bytes ? total_len - header_bytes : 0;
        *header_end = '\0';
        char* status_line = full_response;
        char* space1 = strchr(status_line, ' ');
        if (space1) {
            response->status_code = atoi(space1 + 1);
        }

        response->headers = string_new(full_response);

        /* De-chunk a `Transfer-Encoding: chunked` body so consumers see
         * the decoded payload, not the raw chunk framing
         * (`13\r\n…\r\n0\r\n\r\n`). Without this, any unknown-length /
         * streamed upstream response (no Content-Length) came back
         * framed — corrupting e.g. VCR record-mode tapes
         * (vcr_record_chunked_dechunk_wish.md). Gated on the header, so
         * only chunked bodies — already garbled today — change shape;
         * on malformed framing we keep the raw bytes. */
        const char* body_start = header_end + 4;
        char* te = http_extract_response_header(full_response, "Transfer-Encoding");
        char* dechunked = NULL;
        size_t dechunked_len = 0;
        if (te && http_value_has_chunked(te)) {
            dechunked = http_dechunk(body_start, body_bytes, &dechunked_len);
        }
        free(te);
        if (dechunked) {
            response->body = string_new_with_length(dechunked, dechunked_len);
            free(dechunked);
        } else {
            response->body = string_new_with_length(body_start, body_bytes);
        }
    } else {
        response->body = string_new_with_length(full_response, total_len);
    }

    free(full_response);
    return response;
}

/* Case-insensitive header-name match. Used both when emitting the
 * built-in headers (skip if caller already set one) and when looking
 * up a response header by name. */
static int http_strcaseeq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int header_already_set(HttpRequest* req, const char* name) {
    if (!req) return 0;
    for (HttpHeader* h = req->headers; h; h = h->next) {
        if (http_strcaseeq(h->name, name)) return 1;
    }
    return 0;
}

/* Look up the case-folded value of a response header from the raw
 * header block. Lighter than http_response_header_raw because it
 * works directly on the string buffer rather than the response
 * struct (so the redirect loop below can use it without releasing
 * intermediate responses). Returns a freshly-allocated copy the
 * caller must free, or NULL if the header isn't present. */
static char* http_extract_response_header(const char* hdr_block, const char* name) {
    if (!hdr_block || !name) return NULL;
    size_t name_len = strlen(name);
    const char* p = hdr_block;
    /* Skip the status line. */
    const char* nl = strchr(p, '\n');
    if (nl) p = nl + 1;
    while (*p) {
        const char* line_end = strchr(p, '\n');
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
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
                while (v < p + line_len && (*v == ' ' || *v == '\t')) v++;
                size_t vlen = (size_t)((p + line_len) - v);
                while (vlen > 0 && (v[vlen - 1] == '\r' || v[vlen - 1] == ' ' ||
                                    v[vlen - 1] == '\t')) vlen--;
                char* out = (char*)malloc(vlen + 1);
                if (!out) return NULL;
                memcpy(out, v, vlen);
                out[vlen] = '\0';
                return out;
            }
        }
        if (!line_end) break;
        p = line_end + 1;
    }
    return NULL;
}

/* Case-insensitive: does the Transfer-Encoding value name `chunked`?
 * `chunked` is the final (and in practice only) transfer coding we
 * decode; a comma-list like "gzip, chunked" still matches. */
static int http_value_has_chunked(const char* v) {
    if (!v) return 0;
    for (const char* p = v; *p; p++) {
        if ((*p == 'c' || *p == 'C') &&
            (p[1] == 'h' || p[1] == 'H') && (p[2] == 'u' || p[2] == 'U') &&
            (p[3] == 'n' || p[3] == 'N') && (p[4] == 'k' || p[4] == 'K') &&
            (p[5] == 'e' || p[5] == 'E') && (p[6] == 'd' || p[6] == 'D')) {
            return 1;
        }
    }
    return 0;
}

/* Decode HTTP/1.1 chunked transfer-encoding (RFC 7230 §4.1). `in` /
 * `in_len` is the raw chunk-framed body; writes the decoded length to
 * *out_len and returns a malloc'd buffer (NUL-terminated for
 * convenience; *out_len excludes the NUL). Returns NULL on malformed
 * framing so the caller can fall back to the raw bytes. Binary-safe:
 * the payload is copied by length, never scanned for NUL. Chunk
 * extensions (`<size>;name=val`) are skipped; trailing trailers after
 * the terminating `0` chunk are ignored. */
static char* http_dechunk(const char* in, size_t in_len, size_t* out_len) {
    if (!in || !out_len) return NULL;
    char* out = (char*)malloc(in_len + 1);   /* decoded payload <= input */
    if (!out) return NULL;
    size_t oi = 0;
    size_t i = 0;
    while (i < in_len) {
        size_t sz = 0;
        int saw_hex = 0;
        while (i < in_len) {
            char c = in[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            sz = sz * 16u + (size_t)d;
            saw_hex = 1;
            i++;
        }
        if (!saw_hex) { free(out); return NULL; }   /* expected a hex size */
        /* Skip the rest of the size line (chunk extensions) to the LF. */
        while (i < in_len && in[i] != '\n') i++;
        if (i >= in_len) { free(out); return NULL; } /* no CRLF after size */
        i++;                                          /* consume the '\n' */
        if (sz == 0) break;                           /* terminating chunk */
        if (sz > in_len - i) { free(out); return NULL; } /* truncated chunk */
        memcpy(out + oi, in + i, sz);
        oi += sz;
        i += sz;
        /* Consume the CRLF (or bare LF) that follows the chunk data. */
        if (i < in_len && in[i] == '\r') i++;
        if (i < in_len && in[i] == '\n') i++;
    }
    out[oi] = '\0';
    *out_len = oi;
    return out;
}

/* Resolve a Location-header value against a base URL. The Location
 * may be absolute (`http://other.host/x`), scheme-relative
 * (`//other.host/x`), root-relative (`/x`), or relative
 * (`x`). Returns a malloc'd absolute URL on success, NULL on
 * malformed input. */
static char* http_resolve_location(const char* base_url, const char* location) {
    if (!base_url || !location || !*location) return NULL;
    /* Absolute URL — Location starts with a scheme. */
    if (strstr(location, "://")) return strdup(location);
    /* Need to extract scheme + host[:port] from base_url.
     * parse_url returns 1 on success, 0 on failure — bail when it
     * fails. The earlier round-1 implementation had the check
     * inverted, which produced a bogus "malformed Location header"
     * for every well-formed base URL. Caught by the cases-8-10
     * end-to-end test in test_http_client_v2.ae. */
    char base_host[256], base_path[1024];
    int base_port = 0, base_use_tls = 0;
    if (parse_url(base_url, base_host, sizeof(base_host),
                  &base_port, base_path, sizeof(base_path), &base_use_tls) == 0) {
        return NULL;
    }
    const char* scheme = base_use_tls ? "https" : "http";
    /* Scheme-relative: //host/x → keep base scheme. */
    if (location[0] == '/' && location[1] == '/') {
        size_t need = strlen(scheme) + 1 + strlen(location) + 1;
        char* out = (char*)malloc(need);
        if (!out) return NULL;
        snprintf(out, need, "%s:%s", scheme, location);
        return out;
    }
    /* Root-relative: /x → keep base scheme + host[:port]. */
    if (location[0] == '/') {
        size_t need = strlen(scheme) + 3 + strlen(base_host) + 16 + strlen(location) + 1;
        char* out = (char*)malloc(need);
        if (!out) return NULL;
        if ((base_use_tls && base_port == 443) || (!base_use_tls && base_port == 80)) {
            snprintf(out, need, "%s://%s%s", scheme, base_host, location);
        } else {
            snprintf(out, need, "%s://%s:%d%s", scheme, base_host, base_port, location);
        }
        return out;
    }
    /* Relative path: replace last segment of base_path. */
    char joined_path[1024];
    char* last_slash = strrchr(base_path, '/');
    if (last_slash) {
        size_t prefix_len = (size_t)(last_slash - base_path) + 1;
        if (prefix_len + strlen(location) + 1 > sizeof(joined_path)) return NULL;
        memcpy(joined_path, base_path, prefix_len);
        strcpy(joined_path + prefix_len, location);
    } else {
        snprintf(joined_path, sizeof(joined_path), "/%s", location);
    }
    size_t need = strlen(scheme) + 3 + strlen(base_host) + 16 + strlen(joined_path) + 1;
    char* out = (char*)malloc(need);
    if (!out) return NULL;
    if ((base_use_tls && base_port == 443) || (!base_use_tls && base_port == 80)) {
        snprintf(out, need, "%s://%s%s", scheme, base_host, joined_path);
    } else {
        snprintf(out, need, "%s://%s:%d%s", scheme, base_host, base_port, joined_path);
    }
    return out;
}

/* Strip headers that should not be forwarded across a host change
 * (Authorization, Cookie, Proxy-Authorization). Modifies req in
 * place. Called on each redirect hop where the target host differs
 * from the previous host. */
static void http_strip_cross_host_headers(HttpRequest* req) {
    if (!req) return;
    HttpHeader** link = &req->headers;
    while (*link) {
        HttpHeader* h = *link;
        int strip = http_strcaseeq(h->name, "Authorization") ||
                    http_strcaseeq(h->name, "Cookie") ||
                    http_strcaseeq(h->name, "Proxy-Authorization");
        if (strip) {
            *link = h->next;
            free(h->name); free(h->value); free(h);
        } else {
            link = &h->next;
        }
    }
}

/* v2 entry point. The v1 wrappers below build a throwaway request
 * and call this. Handles redirect-following when the request was
 * configured with max_redirects > 0 (issue #239). */
HttpResponse* http_send_raw(HttpRequest* req) {
    if (!req) return NULL;

    HttpResponse* resp = http_request_internal(req);
    if (!resp) return NULL;

    /* Stash the URL of the originating request as the effective URL.
     * Overwritten below if redirects are followed. */
    if (req->url) resp->effective_url = string_new(req->url);

    /* If redirects aren't enabled, return the first response as-is. */
    if (req->max_redirects <= 0) return resp;

    /* Track visited URLs for loop detection. Bounded by max_redirects
     * + 1 (the original URL). Static array is fine — max_redirects is
     * expected to be small (typically 5-10). */
    char* visited[64];
    int visited_count = 0;
    int max_track = req->max_redirects + 1;
    if (max_track > 64) max_track = 64;
    visited[visited_count++] = strdup(req->url);

    int hops_remaining = req->max_redirects;
    char* current_url = strdup(req->url);

    while (hops_remaining > 0 && resp && resp->status_code >= 300 && resp->status_code < 400) {
        const char* hdrs = resp->headers ? string_to_cstr(resp->headers) : "";
        char* location = http_extract_response_header(hdrs, "Location");
        if (!location) break;  /* 3xx with no Location → return as-is. */

        char* next_url = http_resolve_location(current_url, location);
        free(location);
        if (!next_url) {
            /* Redirect-class error — record on redirect_error so the
             * v2 send_request wrapper preserves the response and the
             * caller can still inspect the terminal 3xx. Issue #239. */
            if (resp->redirect_error) string_release(resp->redirect_error);
            resp->redirect_error = string_new("malformed Location header");
            break;
        }

        /* Reject scheme downgrade: HTTPS origin → HTTP target. */
        int curr_https = strncmp(current_url, "https://", 8) == 0;
        int next_https = strncmp(next_url, "https://", 8) == 0;
        if (curr_https && !next_https) {
            if (resp->redirect_error) string_release(resp->redirect_error);
            resp->redirect_error = string_new(
                "redirect rejected: scheme downgrade (https -> http)");
            free(next_url);
            break;
        }

        /* Loop detection: refuse to revisit a URL within this chain. */
        int looped = 0;
        for (int i = 0; i < visited_count; i++) {
            if (visited[i] && strcmp(visited[i], next_url) == 0) {
                looped = 1;
                break;
            }
        }
        if (looped) {
            if (resp->redirect_error) string_release(resp->redirect_error);
            resp->redirect_error = string_new(
                "redirect loop detected (hop limit may have been exceeded)");
            free(next_url);
            break;
        }

        /* Strip cross-host auth headers if the host changed.
         * parse_url returns 1 on success — same inverted-check bug
         * as http_resolve_location had above; without this fix the
         * strip path never fired in practice (because parse_url
         * always succeeds for well-formed URLs). */
        char curr_host[256], next_host[256], dummy_path[1024];
        int curr_port = 0, next_port = 0, curr_tls = 0, next_tls = 0;
        if (parse_url(current_url, curr_host, sizeof(curr_host), &curr_port,
                      dummy_path, sizeof(dummy_path), &curr_tls) != 0 &&
            parse_url(next_url, next_host, sizeof(next_host), &next_port,
                      dummy_path, sizeof(dummy_path), &next_tls) != 0) {
            if (strcmp(curr_host, next_host) != 0) {
                http_strip_cross_host_headers(req);
            }
        }

        /* Move on. Record the new URL, swap the request URL, send,
         * release the prior response. */
        if (visited_count < max_track) {
            visited[visited_count++] = strdup(next_url);
        }
        free(current_url);
        current_url = strdup(next_url);

        free(req->url);
        req->url = next_url;  /* takes ownership */

        http_response_free(resp);
        resp = http_request_internal(req);
        if (!resp) break;

        hops_remaining--;
    }

    /* If we exited the loop because we ran out of hops while still
     * looking at a 3xx response, surface that as a redirect_error so
     * the caller can inspect the terminal 3xx status / body without
     * the v2 wrapper auto-freeing the response. */
    if (resp && hops_remaining == 0 && resp->status_code >= 300 && resp->status_code < 400) {
        if (resp->redirect_error) string_release(resp->redirect_error);
        resp->redirect_error = string_new("redirect hop limit reached");
    }

    /* Stash the final URL as the effective URL on the response. */
    if (resp) {
        if (resp->effective_url) string_release(resp->effective_url);
        resp->effective_url = string_new(current_url);
    }

    free(current_url);
    for (int i = 0; i < visited_count; i++) free(visited[i]);

    return resp;
}

/* v1 wrappers — thin sugar over the v2 builder. timeout=0 preserves
 * the original "block forever" behaviour callers had before v2. */

HttpResponse* http_get_raw(const char* url) {
    HttpRequest* req = http_request_raw("GET", url);
    if (!req) return NULL;
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

// http_get_raw + per-call timeout. Closes the polling-loop pain
// where one hung site held the actor's whole AnalyzeNext handler
// hostage. Default `http_get_raw` blocks forever for backward
// compatibility; new callers should reach for this variant any time
// the URL is third-party or non-local.
HttpResponse* http_get_with_timeout_raw(const char* url, int timeout_ms) {
    HttpRequest* req = http_request_raw("GET", url);
    if (!req) return NULL;
    if (timeout_ms > 0) {
        // SO_RCVTIMEO / SO_SNDTIMEO storage is integer seconds; round
        // up so callers asking for 50ms get a 1s timeout rather than
        // accidentally disabling the timeout (0 = block forever). The
        // sub-second granularity gap is documented; ms-precision
        // timeouts are a separate change to the underlying field.
        int secs = (timeout_ms + 999) / 1000;
        http_request_set_timeout_raw(req, secs);
    }
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

HttpResponse* http_get_with_timeout_ns_raw(const char* url, int64_t timeout_ns) {
    HttpRequest* req = http_request_raw("GET", url);
    if (!req) return NULL;
    if (http_request_set_timeout_ns_raw(req, timeout_ns) != 0) {
        http_request_free_raw(req);
        return NULL;
    }
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

HttpResponse* http_post_raw(const char* url, const char* body, const char* content_type) {
    HttpRequest* req = http_request_raw("POST", url);
    if (!req) return NULL;
    if (body) {
        http_request_set_body_raw(req, body, (int)strlen(body), content_type);
    }
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

HttpResponse* http_put_raw(const char* url, const char* body, const char* content_type) {
    HttpRequest* req = http_request_raw("PUT", url);
    if (!req) return NULL;
    if (body) {
        http_request_set_body_raw(req, body, (int)strlen(body), content_type);
    }
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

HttpResponse* http_delete_raw(const char* url) {
    HttpRequest* req = http_request_raw("DELETE", url);
    if (!req) return NULL;
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

void http_response_free(HttpResponse* response) {
    if (!response) return;
    if (response->body) string_release(response->body);
    if (response->headers) string_release(response->headers);
    if (response->error) string_release(response->error);
    if (response->redirect_error) string_release(response->redirect_error);
    if (response->effective_url) string_release(response->effective_url);
    free(response);
}

// Response accessors. All NULL-safe: callers can pass a NULL response
// (e.g. from an out-of-memory path) without crashing.

int http_response_status(HttpResponse* response) {
    if (!response) return 0;
    return response->status_code;
}

const char* http_response_body(HttpResponse* response) {
    if (!response || !response->body) return "";
    const char* s = string_to_cstr(response->body);
    return s ? s : "";
}

int http_response_body_length(HttpResponse* response) {
    if (!response || !response->body) return 0;
    return (int)aether_string_length(response->body);
}

const char* http_response_headers(HttpResponse* response) {
    if (!response || !response->headers) return "";
    const char* s = string_to_cstr(response->headers);
    return s ? s : "";
}

const char* http_response_error(HttpResponse* response) {
    if (!response || !response->error) return "";
    const char* s = string_to_cstr(response->error);
    return s ? s : "";
}

int http_response_ok(HttpResponse* response) {
    if (!response) return 0;
    if (response->error) return 0;
    return response->status_code >= 200 && response->status_code < 300;
}

// Legacy accessor aliases — thin wrappers over the short names above.
int http_response_status_code(HttpResponse* response) {
    return http_response_status(response);
}

const char* http_response_body_str(HttpResponse* response) {
    return http_response_body(response);
}

const char* http_response_headers_str(HttpResponse* response) {
    return http_response_headers(response);
}

/* Case-insensitive header lookup. Walks the raw header block stored
 * in response->headers (which still includes the HTTP status line as
 * the first "header"), splits each line at the first `:`, and matches
 * the name. Returns "" when the header isn't found.
 *
 * The returned pointer is into a per-response cache so it remains
 * valid until http_response_free(). Multiple values for the same
 * header are joined with ", " (RFC 7230 §3.2.2).
 *
 * Implementation: lazy single-pass. The cache is a singly-linked
 * list of (name, value) hung off the response — we don't pre-parse
 * into a hashmap because typical responses have <30 headers and the
 * lookup count per response is tiny. */
typedef struct HttpHeaderCache {
    char* name;
    char* value;
    struct HttpHeaderCache* next;
} HttpHeaderCache;

const char* http_response_header_raw(HttpResponse* response, const char* name) {
    if (!response || !name || !*name) return "";
    if (!response->headers) return "";

    /* The response struct doesn't have a parsed-headers field; we
     * cache by attaching to a per-thread arena. Simpler and adequate:
     * just walk the raw block on every call. The cost is O(n) in
     * header bytes, but typical headers are well under 4 KB and the
     * call count per response is small (callers grab the few headers
     * they care about and move on). If a profiler ever shows this in
     * the hot path, swap in a per-response cache.
     *
     * Joining duplicate-named headers into ", "-separated value is
     * done in a thread-local accumulator below. */
    static _Thread_local char tls_joined[8192];
    tls_joined[0] = '\0';
    size_t joined_len = 0;

    const char* hdr = string_to_cstr(response->headers);
    if (!hdr) return "";

    /* Skip the status line (first line — "HTTP/1.1 200 OK"). */
    const char* p = strchr(hdr, '\n');
    if (!p) return "";
    p++;

    while (*p) {
        const char* line_end = strchr(p, '\n');
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
        /* Trim trailing \r if present. */
        if (line_len > 0 && p[line_len - 1] == '\r') line_len--;

        const char* colon = (const char*)memchr(p, ':', line_len);
        if (colon) {
            size_t nlen = (size_t)(colon - p);
            /* Match the header name case-insensitively. */
            if (nlen == strlen(name)) {
                int eq = 1;
                for (size_t i = 0; i < nlen; i++) {
                    char ca = p[i], cb = name[i];
                    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
                    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
                    if (ca != cb) { eq = 0; break; }
                }
                if (eq) {
                    /* Skip ": " then trim leading spaces. */
                    const char* val = colon + 1;
                    size_t vlen = (size_t)((p + line_len) - val);
                    while (vlen > 0 && (*val == ' ' || *val == '\t')) { val++; vlen--; }
                    /* Append to the joined accumulator. */
                    if (joined_len > 0 && joined_len + 2 < sizeof(tls_joined)) {
                        memcpy(tls_joined + joined_len, ", ", 2);
                        joined_len += 2;
                    }
                    if (joined_len + vlen < sizeof(tls_joined)) {
                        memcpy(tls_joined + joined_len, val, vlen);
                        joined_len += vlen;
                        tls_joined[joined_len] = '\0';
                    }
                }
            }
        }

        if (!line_end) break;
        p = line_end + 1;
    }

    return tls_joined;
}

const char* http_response_effective_url_raw(HttpResponse* response) {
    if (!response || !response->effective_url) return "";
    const char* s = string_to_cstr(response->effective_url);
    return s ? s : "";
}

const char* http_response_redirect_error_raw(HttpResponse* response) {
    if (!response || !response->redirect_error) return "";
    const char* s = string_to_cstr(response->redirect_error);
    return s ? s : "";
}

#endif // AETHER_HAS_NETWORKING
