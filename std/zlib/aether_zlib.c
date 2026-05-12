#include "aether_zlib.h"
#include "../string/aether_string.h"
#include "../../runtime/aether_resource_caps.h"

#include <stdlib.h>
#include <string.h>

#ifdef AETHER_HAS_ZLIB
#include <zlib.h>
#endif

/* Unwrap the payload from a `data` argument that may be either an
 * AetherString* or a plain char*. Mirrors the helper in
 * std/fs/aether_fs.c and std/cryptography/aether_cryptography.c —
 * without this dispatch a length-aware AetherString from
 * fs.read_binary would leak its struct header into the stream. */
static inline const unsigned char* zlib_unwrap_bytes(const char* data, int length, size_t* out_len) {
    if (!data) { *out_len = 0; return NULL; }
    if (is_aether_string(data)) {
        const AetherString* s = (const AetherString*)data;
        *out_len = (length >= 0) ? (size_t)length : s->length;
        return (const unsigned char*)s->data;
    }
    *out_len = (length >= 0) ? (size_t)length : strlen(data);
    return (const unsigned char*)data;
}

/* Thread-local buffers for the split-accessor pattern. One pair per
 * direction so a caller can interleave deflate + inflate without the
 * two fighting over a single slot.
 *
 * Resource-caps tracking (#343): both directions remember the
 * allocation capacity alongside the payload length so the matching
 * free goes through `aether_caps_free` with the right byte count.
 * The cap counter stays at current-usage rather than high-water-mark
 * — important for plugin-host sandboxes that re-arm the cap between
 * compressor invocations. Length and capacity differ when the chosen
 * `compressBound` over-allocated relative to the actual compressed
 * size; `_cap` holds the allocator's view (the count that must
 * round-trip to free), `_len` holds the payload's view. */
static _Thread_local unsigned char* tls_deflate_buf = NULL;
static _Thread_local size_t         tls_deflate_cap = 0;
static _Thread_local int            tls_deflate_len = 0;
static _Thread_local unsigned char* tls_inflate_buf = NULL;
static _Thread_local size_t         tls_inflate_cap = 0;
static _Thread_local int            tls_inflate_len = 0;

static void free_deflate_tls(void) {
    if (tls_deflate_buf) {
        aether_caps_free(tls_deflate_buf, tls_deflate_cap);
        tls_deflate_buf = NULL;
    }
    tls_deflate_cap = 0;
    tls_deflate_len = 0;
}
static void free_inflate_tls(void) {
    if (tls_inflate_buf) {
        aether_caps_free(tls_inflate_buf, tls_inflate_cap);
        tls_inflate_buf = NULL;
    }
    tls_inflate_cap = 0;
    tls_inflate_len = 0;
}

int zlib_backend_available(void) {
#ifdef AETHER_HAS_ZLIB
    return 1;
#else
    return 0;
#endif
}

#ifdef AETHER_HAS_ZLIB

int zlib_try_deflate(const char* data, int length, int level) {
    free_deflate_tls();  /* drop any leftover from a previous call */
    if (length < 0) return 0;

    size_t in_len;
    const unsigned char* in = zlib_unwrap_bytes(data, length, &in_len);
    if (in_len > 0 && !in) return 0;

    uLongf bound = compressBound((uLong)in_len);
    size_t alloc_cap = bound > 0 ? (size_t)bound : 1;
    unsigned char* out = (unsigned char*)aether_caps_malloc(alloc_cap);
    if (!out) return 0;

    uLongf out_size = bound;
    int lvl = (level < -1 || level > 9) ? Z_DEFAULT_COMPRESSION : level;
    int rc = compress2(out, &out_size, in, (uLong)in_len, lvl);
    if (rc != Z_OK) { aether_caps_free(out, alloc_cap); return 0; }

    tls_deflate_buf = out;
    tls_deflate_cap = alloc_cap;
    tls_deflate_len = (int)out_size;
    return 1;
}

int zlib_try_gzip_deflate(const char* data, int length, int level) {
    free_deflate_tls();
    if (length < 0) return 0;

    size_t in_len;
    const unsigned char* in = zlib_unwrap_bytes(data, length, &in_len);
    if (in_len > 0 && !in) return 0;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    int lvl = (level < -1 || level > 9) ? Z_DEFAULT_COMPRESSION : level;
    if (deflateInit2(&strm, lvl, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return 0;
    }

    uLong bound = deflateBound(&strm, (uLong)in_len);
    size_t alloc_cap = bound > 0 ? (size_t)bound : 1;
    unsigned char* out = (unsigned char*)aether_caps_malloc(alloc_cap);
    if (!out) { deflateEnd(&strm); return 0; }

    strm.next_in = (Bytef*)in;
    strm.avail_in = (uInt)in_len;
    strm.next_out = out;
    strm.avail_out = (uInt)bound;

    int rc = deflate(&strm, Z_FINISH);
    if (rc != Z_STREAM_END) {
        deflateEnd(&strm);
        aether_caps_free(out, alloc_cap);
        return 0;
    }

    tls_deflate_buf = out;
    tls_deflate_cap = alloc_cap;
    tls_deflate_len = (int)strm.total_out;
    deflateEnd(&strm);
    return 1;
}

int zlib_try_inflate(const char* data, int length) {
    free_inflate_tls();
    if (length < 0) return 0;

    size_t in_len;
    const unsigned char* in = zlib_unwrap_bytes(data, length, &in_len);
    if (in_len == 0) return 0;  /* empty isn't a valid zlib stream */
    if (!in) return 0;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef*)in;
    strm.avail_in = (uInt)in_len;

    if (inflateInit(&strm) != Z_OK) return 0;

    /* 4x the input as a first-cut output guess. Most deflate streams
     * fit in 2-8x their compressed size; grow geometrically if not. */
    size_t cap = in_len * 4;
    if (cap < 64) cap = 64;
    unsigned char* out = (unsigned char*)aether_caps_malloc(cap);
    if (!out) { inflateEnd(&strm); return 0; }

    size_t produced = 0;
    for (;;) {
        strm.next_out = out + produced;
        strm.avail_out = (uInt)(cap - produced);

        int rc = inflate(&strm, Z_NO_FLUSH);
        produced = cap - strm.avail_out;

        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) { aether_caps_free(out, cap); inflateEnd(&strm); return 0; }
        if (strm.avail_out == 0) {
            size_t new_cap = cap * 2;
            if (new_cap < cap) { aether_caps_free(out, cap); inflateEnd(&strm); return 0; }
            unsigned char* bigger = (unsigned char*)aether_caps_realloc(out, cap, new_cap);
            if (!bigger) { aether_caps_free(out, cap); inflateEnd(&strm); return 0; }
            out = bigger;
            cap = new_cap;
        }
    }
    inflateEnd(&strm);

    tls_inflate_buf = out;
    tls_inflate_cap = cap;
    tls_inflate_len = (int)produced;
    return 1;
}

static int inflate_with_window_bits(const char* data, int length, int window_bits) {
    free_inflate_tls();
    if (length < 0) return 0;

    size_t in_len;
    const unsigned char* in = zlib_unwrap_bytes(data, length, &in_len);
    if (in_len == 0) return 0;
    if (!in) return 0;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef*)in;
    strm.avail_in = (uInt)in_len;

    if (inflateInit2(&strm, window_bits) != Z_OK) return 0;

    size_t cap = in_len * 4;
    if (cap < 64) cap = 64;
    unsigned char* out = (unsigned char*)aether_caps_malloc(cap);
    if (!out) { inflateEnd(&strm); return 0; }

    size_t produced = 0;
    for (;;) {
        strm.next_out = out + produced;
        strm.avail_out = (uInt)(cap - produced);

        int rc = inflate(&strm, Z_NO_FLUSH);
        produced = cap - strm.avail_out;

        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) { aether_caps_free(out, cap); inflateEnd(&strm); return 0; }
        if (strm.avail_out == 0) {
            size_t new_cap = cap * 2;
            if (new_cap < cap) { aether_caps_free(out, cap); inflateEnd(&strm); return 0; }
            unsigned char* bigger = (unsigned char*)aether_caps_realloc(out, cap, new_cap);
            if (!bigger) { aether_caps_free(out, cap); inflateEnd(&strm); return 0; }
            out = bigger;
            cap = new_cap;
        }
    }
    inflateEnd(&strm);

    tls_inflate_buf = out;
    tls_inflate_cap = cap;
    tls_inflate_len = (int)produced;
    return 1;
}

int zlib_try_gzip_inflate(const char* data, int length) {
    return inflate_with_window_bits(data, length, 15 + 16);
}

#else /* !AETHER_HAS_ZLIB */

int zlib_try_deflate(const char* data, int length, int level) {
    (void)data; (void)length; (void)level; return 0;
}
int zlib_try_inflate(const char* data, int length) {
    (void)data; (void)length; return 0;
}
int zlib_try_gzip_deflate(const char* data, int length, int level) {
    (void)data; (void)length; (void)level; return 0;
}
int zlib_try_gzip_inflate(const char* data, int length) {
    (void)data; (void)length; return 0;
}

#endif /* AETHER_HAS_ZLIB */

const char* zlib_get_deflate_bytes(void) {
    return (const char*)(tls_deflate_buf ? tls_deflate_buf : (unsigned char*)"");
}
int zlib_get_deflate_length(void) { return tls_deflate_len; }
void zlib_release_deflate(void)   { free_deflate_tls(); }

const char* zlib_get_inflate_bytes(void) {
    return (const char*)(tls_inflate_buf ? tls_inflate_buf : (unsigned char*)"");
}
int zlib_get_inflate_length(void) { return tls_inflate_len; }
void zlib_release_inflate(void)   { free_inflate_tls(); }
