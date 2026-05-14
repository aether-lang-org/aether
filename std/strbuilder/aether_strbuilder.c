#include "aether_strbuilder.h"
#include "../string/aether_string.h"
#include "../../runtime/aether_resource_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AETHER_STRBUILDER_DEFAULT_CAP 64

struct AetherStrBuilder {
    char*  data;
    size_t length;
    size_t capacity;
};

/* Grow the underlying buffer so it has at least `min_capacity` bytes.
 * Doubles each step to keep amortised-O(1) append cost; falls back to
 * `min_capacity` directly once doubling stops being enough or would
 * overflow. Returns 1 on success, 0 on OOM / cap exceeded. */
static int strbuilder_reserve(AetherStrBuilder* b, size_t min_capacity) {
    if (!b) return 0;
    if (b->capacity >= min_capacity) return 1;
    size_t new_cap = b->capacity ? b->capacity : AETHER_STRBUILDER_DEFAULT_CAP;
    while (new_cap < min_capacity) {
        size_t doubled = new_cap * 2;
        if (doubled < new_cap) {
            /* overflow — fall back to the requested size */
            new_cap = min_capacity;
            break;
        }
        new_cap = doubled;
    }
    char* new_data = (char*)aether_caps_realloc(b->data, b->capacity, new_cap);
    if (!new_data) return 0;
    b->data = new_data;
    b->capacity = new_cap;
    return 1;
}

AetherStrBuilder* aether_strbuilder_new(int cap_hint) {
    AetherStrBuilder* b = (AetherStrBuilder*)aether_caps_malloc(sizeof(AetherStrBuilder));
    if (!b) return NULL;
    b->data = NULL;
    b->length = 0;
    b->capacity = 0;
    size_t cap = (cap_hint > 0) ? (size_t)cap_hint : AETHER_STRBUILDER_DEFAULT_CAP;
    if (!strbuilder_reserve(b, cap)) {
        aether_caps_free(b, sizeof(AetherStrBuilder));
        return NULL;
    }
    return b;
}

int aether_strbuilder_append(AetherStrBuilder* b, const void* s) {
    if (!b || !s) return 0;
    /* Length comes from the AetherString header if present, else
     * strlen — meaning content with embedded NULs handed in as a
     * plain `const char*` will truncate. Callers who need binary-safe
     * append should use append_n. */
    size_t n = is_aether_string(s)
        ? aether_string_length(s)
        : strlen((const char*)s);
    return aether_strbuilder_append_n(b, s, (int)n);
}

int aether_strbuilder_append_n(AetherStrBuilder* b, const void* s, int n) {
    if (!b || !s) return 0;
    if (n < 0) return 0;
    if (n == 0) return 1;
    size_t need = b->length + (size_t)n;
    if (need < b->length) return 0;  /* overflow */
    if (!strbuilder_reserve(b, need)) return 0;
    const char* payload = aether_string_data(s);
    if (!payload) return 0;
    memcpy(b->data + b->length, payload, (size_t)n);
    b->length = need;
    return 1;
}

int aether_strbuilder_append_byte(AetherStrBuilder* b, int c) {
    if (!b) return 0;
    size_t need = b->length + 1;
    if (!strbuilder_reserve(b, need)) return 0;
    b->data[b->length] = (char)(c & 0xff);
    b->length = need;
    return 1;
}

int aether_strbuilder_append_int(AetherStrBuilder* b, int v) {
    if (!b) return 0;
    char numbuf[32];
    int nlen = snprintf(numbuf, sizeof(numbuf), "%d", v);
    if (nlen < 0 || nlen >= (int)sizeof(numbuf)) return 0;
    return aether_strbuilder_append_n(b, numbuf, nlen);
}

int aether_strbuilder_length(AetherStrBuilder* b) {
    if (!b) return -1;
    return (int)b->length;
}

void* aether_strbuilder_finish(AetherStrBuilder* b) {
    if (!b) return NULL;
    /* Hand the data buffer off to the caller as a plain libc-freeable
     * char* and free only the wrapper. This matches the @heap-extern
     * contract used by the rest of the stdlib (cf. make_owned in
     * tests/integration/extern_single_value_heap): the
     * heap-string-tracker's reassignment-wrapper emits a plain libc
     * free on the previous value, which on an AetherString* would
     * free the 24-byte header struct and dangle the data buffer
     * (the uniform-heap return shim passes is_heap=1 through
     * unchanged for heap-flagged returns, so we can't piggy-back on
     * its struct-aware path either). A plain char* sidesteps both:
     * libc-free reclaims the whole allocation, the cap drift on the
     * single transfer is acceptable (caps API explicitly allows
     * libc-free of caps-malloc'd memory, header-comment in
     * runtime/aether_resource_caps.h:89-94).
     *
     * Trade-off: the returned char* has no length-bearing header, so
     * downstream operations that use strlen will truncate at the
     * first NUL. Binary content with embedded NULs needs append_n
     * on the input side AND length-aware consumers on the output
     * side (e.g. string_length_n, string_substring_n) if the caller
     * carries the length themselves. The ASCII / UTF-8 case — JSON,
     * log lines, templates, paths — is the overwhelming majority of
     * the motivating use cases. */
    if (!b->data) {
        char* empty = (char*)aether_caps_malloc(1);
        aether_caps_free(b, sizeof(AetherStrBuilder));
        if (!empty) return NULL;
        empty[0] = '\0';
        return empty;
    }
    /* Grow to fit the NUL terminator if the buffer is exactly full. */
    if (b->length + 1 > b->capacity) {
        if (!strbuilder_reserve(b, b->length + 1)) {
            aether_caps_free(b->data, b->capacity);
            aether_caps_free(b, sizeof(AetherStrBuilder));
            return NULL;
        }
    }
    b->data[b->length] = '\0';
    char* out = b->data;
    aether_caps_free(b, sizeof(AetherStrBuilder));
    return out;
}

void aether_strbuilder_free(AetherStrBuilder* b) {
    if (!b) return;
    aether_caps_free(b->data, b->capacity);
    aether_caps_free(b, sizeof(AetherStrBuilder));
}
