/* Copyright (c) 2026 Aether Developers. */
/* BytesCursor — a forward read-position over a borrowed AetherBytes.
 *
 * The cursor holds a pointer to the AetherBytes plus a read offset; it
 * does not copy the data and does not own the buffer (see the BORROW
 * note in the header). All reads go through the public aether_bytes_get
 * / aether_bytes_length accessors so the cursor never pokes the
 * AetherBytes internals and always honours the buffer's logical
 * length. */

#include "aether_bytes_cursor.h"
#include "../../../runtime/aether_resource_caps.h"

struct BytesCursor {
    AetherBytes* bytes;   /* borrowed; cursor must not outlive it */
    int          len;     /* snapshot of the buffer's logical length */
    int          pos;     /* current read offset, in [0, len]        */
};

BytesCursor* bytes_cursor_new(AetherBytes* b) {
    if (!b) return NULL;
    int len = aether_bytes_length(b);
    if (len < 0) return NULL;
    BytesCursor* c = (BytesCursor*)aether_caps_malloc(sizeof(*c));
    if (!c) return NULL;
    c->bytes = b;
    c->len = len;
    c->pos = 0;
    return c;
}

int bytes_cursor_read_u8(BytesCursor* c) {
    if (!c || c->pos >= c->len) return -1;
    int v = aether_bytes_get(c->bytes, c->pos);
    if (v < 0) return -1;
    c->pos += 1;
    return v;
}

int bytes_cursor_read_be_u16(BytesCursor* c) {
    if (!c || c->pos + 2 > c->len) return -1;
    int hi = aether_bytes_get(c->bytes, c->pos);
    int lo = aether_bytes_get(c->bytes, c->pos + 1);
    if (hi < 0 || lo < 0) return -1;
    c->pos += 2;
    return (hi << 8) | lo;
}

int bytes_cursor_read_be_u32(BytesCursor* c) {
    if (!c || c->pos + 4 > c->len) return -1;
    unsigned int v = 0;
    for (int i = 0; i < 4; i++) {
        int b = aether_bytes_get(c->bytes, c->pos + i);
        if (b < 0) return -1;
        v = (v << 8) | (unsigned int)b;
    }
    c->pos += 4;
    return (int)v;
}

long long bytes_cursor_read_be_u64(BytesCursor* c) {
    if (!c || c->pos + 8 > c->len) return -1;
    unsigned long long v = 0;
    for (int i = 0; i < 8; i++) {
        int b = aether_bytes_get(c->bytes, c->pos + i);
        if (b < 0) return -1;
        v = (v << 8) | (unsigned long long)b;
    }
    c->pos += 8;
    return (long long)v;
}

int bytes_cursor_read_le_u16(BytesCursor* c) {
    if (!c || c->pos + 2 > c->len) return -1;
    int lo = aether_bytes_get(c->bytes, c->pos);
    int hi = aether_bytes_get(c->bytes, c->pos + 1);
    if (lo < 0 || hi < 0) return -1;
    c->pos += 2;
    return (hi << 8) | lo;
}

int bytes_cursor_read_le_u32(BytesCursor* c) {
    if (!c || c->pos + 4 > c->len) return -1;
    unsigned int v = 0;
    for (int i = 0; i < 4; i++) {
        int b = aether_bytes_get(c->bytes, c->pos + i);
        if (b < 0) return -1;
        v |= (unsigned int)b << (8 * i);
    }
    c->pos += 4;
    return (int)v;
}

long long bytes_cursor_read_le_u64(BytesCursor* c) {
    if (!c || c->pos + 8 > c->len) return -1;
    unsigned long long v = 0;
    for (int i = 0; i < 8; i++) {
        int b = aether_bytes_get(c->bytes, c->pos + i);
        if (b < 0) return -1;
        v |= (unsigned long long)b << (8 * i);
    }
    c->pos += 8;
    return (long long)v;
}

AetherBytes* bytes_cursor_read_slice(BytesCursor* c, int n) {
    if (!c || n < 0 || c->pos + n > c->len) return NULL;
    AetherBytes* out = aether_bytes_new(n);
    if (!out) return NULL;
    if (n > 0) {
        if (!aether_bytes_copy_from_bytes(out, 0, c->bytes, c->pos, n)) {
            aether_bytes_free(out);
            return NULL;
        }
    }
    c->pos += n;
    return out;
}

int bytes_cursor_remaining(BytesCursor* c) {
    if (!c) return 0;
    return c->len - c->pos;
}

int bytes_cursor_peek(BytesCursor* c) {
    if (!c || c->pos >= c->len) return -1;
    return aether_bytes_get(c->bytes, c->pos);
}

int bytes_cursor_eof(BytesCursor* c) {
    if (!c) return 1;
    return c->pos >= c->len ? 1 : 0;
}

int bytes_cursor_pos(BytesCursor* c) {
    return c ? c->pos : -1;
}

void bytes_cursor_seek(BytesCursor* c, int pos) {
    if (!c) return;
    if (pos < 0) pos = 0;
    if (pos > c->len) pos = c->len;
    c->pos = pos;
}

void bytes_cursor_free(BytesCursor* c) {
    if (!c) return;
    /* Cursor borrows its AetherBytes — do NOT free c->bytes here. Only
     * the cursor struct is ours to reclaim. */
    aether_caps_free(c, sizeof(*c));
}
