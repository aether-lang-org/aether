/* Copyright (c) 2026 Aether Developers. */
#include "aether_tracking.h"

#include <stdlib.h>
#include <stdio.h>

/* Live heap pointers are never 0 or 1, so those double as the empty and
 * tombstone slot sentinels; a real key colliding with them would corrupt
 * the map. */
#define TRACK_EMPTY ((void*)0)
#define TRACK_TOMB  ((void*)1)

typedef struct {
    void*  key;
    size_t size;
    long   seq;
} TrackEntry;

/* handle is the first member so the returned AetherAllocator* aliases the
 * whole struct and handle.data can point back at it. */
typedef struct AetherTracking {
    AetherAllocator  handle;
    AetherAllocator* inner;
    TrackEntry* entries;
    size_t cap;
    size_t live;
    size_t used;
    size_t total_bytes;
    long   seq;
} AetherTracking;

static size_t track_hash(void* p, size_t cap) {
    size_t h = (size_t)p;
    h ^= h >> 33;
    h *= (size_t)0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h & (cap - 1);
}

static void track_put_raw(TrackEntry* entries, size_t cap,
                          void* key, size_t size, long seq) {
    size_t i = track_hash(key, cap);
    while (entries[i].key != TRACK_EMPTY) i = (i + 1) & (cap - 1);
    entries[i].key = key;
    entries[i].size = size;
    entries[i].seq = seq;
}

static int track_grow(AetherTracking* t) {
    size_t new_cap = t->cap ? t->cap * 2 : 64;
    TrackEntry* ne = (TrackEntry*)calloc(new_cap, sizeof(TrackEntry));
    if (!ne) return 0;
    for (size_t i = 0; i < t->cap; i++) {
        void* k = t->entries[i].key;
        if (k != TRACK_EMPTY && k != TRACK_TOMB)
            track_put_raw(ne, new_cap, k, t->entries[i].size, t->entries[i].seq);
    }
    free(t->entries);
    t->entries = ne;
    t->cap = new_cap;
    t->used = t->live;
    return 1;
}

static void track_insert(AetherTracking* t, void* key, size_t size) {
    if (t->used + 1 > (t->cap >> 1) + (t->cap >> 2)) {
        if (!track_grow(t)) return;
    }
    size_t i = track_hash(key, t->cap);
    size_t tomb = t->cap;
    while (t->entries[i].key != TRACK_EMPTY) {
        if (t->entries[i].key == TRACK_TOMB) { if (tomb == t->cap) tomb = i; }
        else if (t->entries[i].key == key) { t->entries[i].size = size; return; }
        i = (i + 1) & (t->cap - 1);
    }
    if (tomb != t->cap) i = tomb; else t->used++;
    t->entries[i].key = key;
    t->entries[i].size = size;
    t->entries[i].seq = t->seq++;
    t->live++;
    t->total_bytes += size;
}

static size_t track_remove(AetherTracking* t, void* key) {
    if (t->cap == 0) return 0;
    size_t i = track_hash(key, t->cap);
    while (t->entries[i].key != TRACK_EMPTY) {
        if (t->entries[i].key == key) {
            size_t sz = t->entries[i].size;
            t->entries[i].key = TRACK_TOMB;
            t->live--;
            t->total_bytes -= sz;
            return sz;
        }
        i = (i + 1) & (t->cap - 1);
    }
    return 0;
}

static void* track_alloc(void* data, size_t size) {
    AetherTracking* t = (AetherTracking*)data;
    void* p = aether_allocator_alloc(t->inner, size);
    if (p) track_insert(t, p, size);
    return p;
}

static void track_free(void* data, void* ptr, size_t size) {
    AetherTracking* t = (AetherTracking*)data;
    if (ptr) track_remove(t, ptr);
    aether_allocator_free(t->inner, ptr, size);
}

static void* track_realloc(void* data, void* ptr, size_t old_size, size_t new_size) {
    AetherTracking* t = (AetherTracking*)data;
    if (!ptr) return track_alloc(data, new_size);
    if (new_size == 0) { track_free(data, ptr, old_size); return NULL; }
    void* np = aether_allocator_realloc(t->inner, ptr, old_size, new_size);
    if (!np) return NULL;
    track_remove(t, ptr);
    track_insert(t, np, new_size);
    return np;
}

AetherAllocator* aether_tracking_wrap(AetherAllocator* inner) {
    AetherTracking* t = (AetherTracking*)calloc(1, sizeof(AetherTracking));
    if (!t) return NULL;
    t->handle.alloc_fn   = track_alloc;
    t->handle.realloc_fn = track_realloc;
    t->handle.free_fn    = track_free;
    t->handle.data       = t;
    t->inner = inner ? inner : aether_allocator_system();
    return &t->handle;
}

int aether_tracking_count(AetherAllocator* t) {
    if (!t) return -1;
    return (int)((AetherTracking*)t)->live;
}

long aether_tracking_bytes(AetherAllocator* t) {
    if (!t) return -1;
    return (long)((AetherTracking*)t)->total_bytes;
}

int aether_tracking_report(AetherAllocator* t) {
    if (!t) return -1;
    AetherTracking* st = (AetherTracking*)t;
    if (st->live == 0) {
        fprintf(stderr, "[tracking] no leaks (0 live allocations)\n");
        return 0;
    }
    fprintf(stderr, "[tracking] %zu live allocation(s), %zu bytes:\n",
            st->live, st->total_bytes);
    for (size_t i = 0; i < st->cap; i++) {
        void* k = st->entries[i].key;
        if (k != TRACK_EMPTY && k != TRACK_TOMB)
            fprintf(stderr, "  leak #%ld: %zu bytes\n",
                    st->entries[i].seq, st->entries[i].size);
    }
    return (int)st->live;
}

void aether_tracking_destroy(AetherAllocator* t) {
    if (!t) return;
    AetherTracking* st = (AetherTracking*)t;
    free(st->entries);
    free(st);
}
