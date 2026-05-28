#include "aether_bytes.h"
#include "../string/aether_string.h"

#include <stdlib.h>
#include <string.h>

struct AetherBytes {
    size_t length;
    size_t capacity;
    char*  data;
};

/* Grow the underlying buffer so it has at least `min_capacity` bytes.
 * Doubles each time to keep amortised-O(1) append cost; falls back to
 * `min_capacity` itself once doubling stops being enough. Returns 1
 * on success, 0 on OOM. */
static int bytes_reserve(AetherBytes* b, size_t min_capacity) {
    if (!b) return 0;
    if (b->capacity >= min_capacity) return 1;
    size_t new_cap = b->capacity ? b->capacity : 16;
    while (new_cap < min_capacity) {
        size_t doubled = new_cap * 2;
        if (doubled < new_cap) {
            /* overflow guard — fall back to the requested size */
            new_cap = min_capacity;
            break;
        }
        new_cap = doubled;
    }
    char* new_data = (char*)realloc(b->data, new_cap);
    if (!new_data) return 0;
    /* Zero the freshly-allocated tail so set() at non-contiguous
     * indices yields zero-filled gaps rather than uninitialised
     * memory. */
    if (new_cap > b->capacity) {
        memset(new_data + b->capacity, 0, new_cap - b->capacity);
    }
    b->data = new_data;
    b->capacity = new_cap;
    return 1;
}

AetherBytes* aether_bytes_new(int initial_capacity) {
    if (initial_capacity < 0) return NULL;
    AetherBytes* b = (AetherBytes*)malloc(sizeof(AetherBytes));
    if (!b) return NULL;
    b->length = 0;
    b->capacity = 0;
    b->data = NULL;
    if (initial_capacity > 0 && !bytes_reserve(b, (size_t)initial_capacity)) {
        free(b);
        return NULL;
    }
    return b;
}

int aether_bytes_length(AetherBytes* b) {
    if (!b) return -1;
    return (int)b->length;
}

int aether_bytes_set(AetherBytes* b, int index, int byte) {
    if (!b || index < 0) return 0;
    size_t needed = (size_t)index + 1;
    if (!bytes_reserve(b, needed)) return 0;
    /* If we're writing past the current tail, the bytes between the
     * old tail and `index` are already zero-filled by bytes_reserve
     * (which zeroes any newly-allocated tail). Update the logical
     * length to cover the new write. */
    b->data[index] = (char)(byte & 0xff);
    if (needed > b->length) b->length = needed;
    return 1;
}

int aether_bytes_get(AetherBytes* b, int index) {
    if (!b || index < 0) return -1;
    if ((size_t)index >= b->length) return -1;
    return (int)(unsigned char)b->data[index];
}

int aether_bytes_set_le16(AetherBytes* b, int index, int value) {
    if (!b || index < 0) return 0;
    size_t needed = (size_t)index + 2;
    if (needed < (size_t)index) return 0;  /* overflow */
    if (!bytes_reserve(b, needed)) return 0;
    b->data[index]     = (char)(value & 0xff);
    b->data[index + 1] = (char)((value >> 8) & 0xff);
    if (needed > b->length) b->length = needed;
    return 1;
}

int aether_bytes_get_le16(AetherBytes* b, int index) {
    if (!b || index < 0) return -1;
    if ((size_t)index + 2 > b->length) return -1;
    unsigned int v = (unsigned char)b->data[index]
                   | ((unsigned int)(unsigned char)b->data[index + 1] << 8);
    return (int)v;
}

int aether_bytes_set_le32(AetherBytes* b, int index, int value) {
    if (!b || index < 0) return 0;
    size_t needed = (size_t)index + 4;
    if (needed < (size_t)index) return 0;  /* overflow */
    if (!bytes_reserve(b, needed)) return 0;
    unsigned int u = (unsigned int)value;
    b->data[index]     = (char)(u & 0xff);
    b->data[index + 1] = (char)((u >> 8) & 0xff);
    b->data[index + 2] = (char)((u >> 16) & 0xff);
    b->data[index + 3] = (char)((u >> 24) & 0xff);
    if (needed > b->length) b->length = needed;
    return 1;
}

int aether_bytes_get_le32(AetherBytes* b, int index) {
    if (!b || index < 0) return -1;
    if ((size_t)index + 4 > b->length) return -1;
    unsigned int v = (unsigned char)b->data[index]
                   | ((unsigned int)(unsigned char)b->data[index + 1] << 8)
                   | ((unsigned int)(unsigned char)b->data[index + 2] << 16)
                   | ((unsigned int)(unsigned char)b->data[index + 3] << 24);
    return (int)v;
}

int aether_bytes_copy_from_string(AetherBytes* b, int dst,
                                  const void* src, int src_len) {
    if (!b || dst < 0 || !src || src_len < 0) return 0;
    if (src_len == 0) return 1;
    size_t needed = (size_t)dst + (size_t)src_len;
    if (needed < (size_t)dst) return 0;  /* overflow */
    if (!bytes_reserve(b, needed)) return 0;
    /* Read the payload via aether_string_data so AetherString* and
     * plain char* are both accepted. memcpy is fine here — `src` and
     * `b->data` are distinct allocations. */
    const char* payload = aether_string_data(src);
    if (!payload) return 0;
    memcpy(b->data + dst, payload, (size_t)src_len);
    if (needed > b->length) b->length = needed;
    return 1;
}

int aether_bytes_copy_from_bytes(AetherBytes* dst, int dst_off,
                                 AetherBytes* src, int src_off,
                                 int length) {
    if (!dst || !src || dst_off < 0 || src_off < 0 || length < 0) return 0;
    if (length == 0) return 1;
    /* Source range must fully exist; this is a plain copy, not the
     * forward-overlap RLE trick. */
    if ((size_t)src_off + (size_t)length > src->length) return 0;
    if ((size_t)src_off + (size_t)length < (size_t)src_off) return 0;  /* overflow */
    size_t needed = (size_t)dst_off + (size_t)length;
    if (needed < (size_t)dst_off) return 0;  /* overflow */
    if (!bytes_reserve(dst, needed)) return 0;
    /* memmove is correct even when dst == src buffers happen to share
     * a backing allocation (they shouldn't, but defensive); for the
     * deliberate in-buffer forward-overlap RLE pattern, callers must
     * use aether_bytes_copy_within instead. */
    memmove(dst->data + dst_off, src->data + src_off, (size_t)length);
    if (needed > dst->length) dst->length = needed;
    return 1;
}

int aether_bytes_copy_within(AetherBytes* b, int dst, int src, int length) {
    if (!b || dst < 0 || src < 0 || length < 0) return 0;
    if (length == 0) return 1;
    /* `src` itself must point into the current logical length — we
     * have to be able to read at least the first byte. But unlike
     * memcpy, we DO allow `src + length` to extend past the current
     * tail when `dst > src`: this is the RLE-expansion case where
     * each later iteration reads a byte the loop just wrote.
     *
     * Concretely: with src=0, dst=2, length=4, the loop reads
     * data[0..3] while writing data[2..5]. At i=2 it reads data[2]
     * — which iteration i=0 just wrote. That's how runs of repeated
     * bytes get encoded.
     *
     * For the RLE pattern to work, `dst >= src` is the constraint:
     * writes must chase reads forward, never get ahead of them. If
     * `dst < src`, we're doing a simple non-overlapping copy and
     * there's no RLE story to worry about. */
    if ((size_t)src >= b->length) return 0;
    if (dst > src) {
        /* Each i must read a byte the loop has guaranteed exists by
         * step i. The byte at src+i is either part of the original
         * payload (i < length - (dst - src)) or has been written by
         * iteration i - (dst - src) of the loop. So as long as
         * dst - src >= 1, every read targets an already-written
         * position by the time it happens. */
    } else {
        /* dst <= src: non-overlapping or overwrite-in-place. The
         * read region must be fully present. */
        if ((size_t)src + (size_t)length > b->length) return 0;
    }
    size_t needed = (size_t)dst + (size_t)length;
    if (needed < (size_t)dst) return 0;  /* overflow */
    if (!bytes_reserve(b, needed)) return 0;
    /* DELIBERATELY forward byte-by-byte. memmove() chooses a safe
     * direction; we want unsafe-forward so the RLE pattern works. */
    for (int i = 0; i < length; i++) {
        b->data[dst + i] = b->data[src + i];
    }
    if (needed > b->length) b->length = needed;
    return 1;
}

void* aether_bytes_finish(AetherBytes* b, int length) {
    if (!b) return NULL;
    size_t use_length = (length < 0) ? 0 : (size_t)length;
    if (use_length > b->length) use_length = b->length;
    /* Hand the bytes off to an AetherString. We can't transfer the
     * existing buffer directly because string_new_with_length
     * allocates its own (and frees on release_string). Copy +
     * destroy the AetherBytes — small overhead vs the alternative
     * of teaching the string layer to adopt an external buffer. */
    void* wrapped = (void*)string_new_with_length(b->data, use_length);
    aether_bytes_free(b);
    return wrapped;
}

void aether_bytes_free(AetherBytes* b) {
    if (!b) return;
    free(b->data);
    free(b);
}
