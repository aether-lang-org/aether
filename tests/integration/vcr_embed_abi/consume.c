/*
 * consume.c — C host for the VCR embedding C-ABI smoke test
 * (vcr_embed_abi_wish.md Part B). dlopens the libvcr shared object built
 * from std/http/server/vcr/embed.ae and drives a playback round-trip:
 *
 *   start_playback(smoke.tape, 127.0.0.1, 0)  → opaque handle
 *   port()      → OS-assigned port (> 0)
 *   base_url()  → "http://127.0.0.1:<port>"   (caller-owned string)
 *   tape_length → 1
 *   raw-socket GET /ok against the running server → body "ok-body"
 *   last_kind() → 0 (KIND_OK)
 *   free_string + stop
 *
 * This is the contract servirtium-dotnet's P/Invoke layer mirrors. The
 * .so is self-contained (libaether is statically linked in by
 * `ae build --emit=lib`), so the driver only needs -ldl.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

typedef void* (*start_playback_fn)(const char*, const char*, const char*, int);
typedef int   (*port_fn)(void*);
typedef char* (*base_url_fn)(void*, const char*);
typedef int   (*tape_length_fn)(void);
typedef int   (*last_kind_fn)(void);
typedef void  (*stop_fn)(void*);
typedef void  (*free_string_fn)(char*);

static void* sym(void* h, const char* name) {
    void* p = dlsym(h, name);
    if (!p) { fprintf(stderr, "FAIL: symbol %s not found: %s\n", name, dlerror()); exit(1); }
    return p;
}

/* Minimal HTTP/1.1 GET over a raw socket; copies the response into buf. */
static int http_get(int port, const char* path, char* buf, size_t cap) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); return -1; }

    char req[512];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path);
    if (write(fd, req, (size_t)n) != n) { close(fd); return -1; }

    size_t total = 0;
    ssize_t r;
    while (total < cap - 1 && (r = read(fd, buf + total, cap - 1 - total)) > 0) {
        total += (size_t)r;
    }
    buf[total] = '\0';
    close(fd);
    return (int)total;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <libvcr.so> <tape_path>\n", argv[0]);
        return 2;
    }
    void* h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "FAIL: dlopen(%s): %s\n", argv[1], dlerror()); return 1; }

    start_playback_fn start_playback = (start_playback_fn)sym(h, "aether_vcr_embed_start_playback");
    port_fn          get_port    = (port_fn)         sym(h, "aether_vcr_embed_port");
    base_url_fn      base_url    = (base_url_fn)     sym(h, "aether_vcr_embed_base_url");
    tape_length_fn   tape_length = (tape_length_fn)  sym(h, "aether_vcr_embed_tape_length");
    last_kind_fn     last_kind   = (last_kind_fn)    sym(h, "aether_vcr_embed_last_kind");
    stop_fn          stop        = (stop_fn)         sym(h, "aether_vcr_embed_stop");
    free_string_fn   free_string = (free_string_fn)  sym(h, "aether_vcr_embed_free_string");

    void* srv = start_playback("smoke", argv[2], "127.0.0.1", 0);
    if (!srv) { fprintf(stderr, "FAIL: start_playback returned NULL\n"); return 1; }

    int port = get_port(srv);
    if (port <= 0) { fprintf(stderr, "FAIL: port() = %d (OS-assigned port unresolved)\n", port); return 1; }

    char* url = base_url(srv, "127.0.0.1");
    char expect_url[64];
    snprintf(expect_url, sizeof(expect_url), "http://127.0.0.1:%d", port);
    if (!url || strcmp(url, expect_url) != 0) {
        fprintf(stderr, "FAIL: base_url() = %s, expected %s\n", url ? url : "(null)", expect_url);
        return 1;
    }
    free_string(url);

    int tl = tape_length();
    if (tl != 1) { fprintf(stderr, "FAIL: tape_length() = %d, expected 1\n", tl); return 1; }

    char resp[8192];
    if (http_get(port, "/ok", resp, sizeof(resp)) <= 0) {
        fprintf(stderr, "FAIL: HTTP GET /ok against the embedded server failed\n");
        return 1;
    }
    if (!strstr(resp, "ok-body")) {
        fprintf(stderr, "FAIL: GET /ok response did not contain recorded body 'ok-body'\n--- response:\n%s\n", resp);
        return 1;
    }
    if (last_kind() != 0) {
        fprintf(stderr, "FAIL: last_kind() = %d after a matching GET, expected 0 (KIND_OK)\n", last_kind());
        return 1;
    }

    stop(srv);
    dlclose(h);
    printf("OK: VCR embed playback round-trip via dlopen (port, base_url, GET /ok, last_kind)\n");
    return 0;
}
