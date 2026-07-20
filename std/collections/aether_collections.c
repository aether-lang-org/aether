#include "aether_collections.h"
#include "../../runtime/aether_resource_caps.h"
#include "../../runtime/aether_value_kind.h"
#include "../string/aether_string.h"
#include "../alloc/aether_alloc.h"
#include <stdlib.h>
#include <string.h>

/* std.list backing-allocator dispatch (#1045): NULL keeps the default
 * cap-aware path unchanged; a non-NULL handle routes the list's own
 * struct/items/owned_flags memory through it. */
static void* cl_alloc(AetherAllocator* a, size_t n) {
    return a ? aether_allocator_alloc(a, n) : aether_caps_malloc(n);
}
static void* cl_calloc(AetherAllocator* a, size_t n, size_t sz) {
    if (!a) return aether_caps_calloc(n, sz);
    size_t total = n * sz;
    void* p = aether_allocator_alloc(a, total);
    if (p) memset(p, 0, total);
    return p;
}
static void* cl_realloc(AetherAllocator* a, void* p, size_t oldn, size_t newn) {
    return a ? aether_allocator_realloc(a, p, oldn, newn)
             : aether_caps_realloc(p, oldn, newn);
}
static void cl_free(AetherAllocator* a, void* p, size_t n) {
    if (a) aether_allocator_free(a, p, n);
    else aether_caps_free(p, n);
}

/* The leading uint32_t kind-magic on both ArrayList and HashMap is
 * the discriminator used by aether_value_kind() in runtime/
 * aether_config.c. Hosts holding an opaque AetherValue* can probe
 * it safely (low-address guard + magic check) to discriminate map /
 * list / scalar without a schema lookup. The struct layout is
 * private to this TU; the magic must remain the first field.
 *
 * `owns_string_elements` (#467): set by `list_add_string_owned`
 * when the codegen-emitted call site recognises the added value
 * as a heap-string expression. While set, `list_free` walks every
 * element and calls `string_release` before freeing the backing
 * array — so heap strings stored in the list are reclaimed cleanly
 * rather than leaking. Mixed lists (heap-string + non-string
 * elements) aren't supported by this flag — Aether-side
 * codegen always knows the static type at each call site and
 * routes accordingly. */
struct ArrayList {
    uint32_t _kind_magic;       /* = AETHER_KIND_LIST_MAGIC */
    void** items;
    int size;
    int capacity;
    /* #467: per-element heap-string-ownership tracking. Parallel
     * array to `items` (allocated lazily alongside the items
     * array's first grow). `owned_flags[i] == 1` means
     * `items[i]` was added via list_add_string_owned and should
     * be released by list_free; `owned_flags[i] == 0` means the
     * caller retains ownership (literals, int-cast-to-ptr, etc.).
     * The two paths can coexist in a single list — mixing is
     * safe by construction. NULL on a fresh list that has never
     * had an owned-put; first owned-put allocates it. */
    int* owned_flags;
    AetherAllocator* alloc;   /* #1045: NULL = default cap-aware path */
};

ArrayList* list_new_in(AetherAllocator* alloc) {
    ArrayList* list = (ArrayList*)cl_alloc(alloc, sizeof(ArrayList));
    if (!list) return NULL;
    list->_kind_magic = AETHER_KIND_LIST_MAGIC;
    list->items = NULL;
    list->size = 0;
    list->capacity = 0;
    list->owned_flags = NULL;
    list->alloc = alloc;
    return list;
}

ArrayList* list_new() {
    return list_new_in(NULL);
}

// list_add_raw returns 1 on success, 0 on failure (realloc error or
// null list). The Aether wrapper `list.add` in std.list/std.collections
// turns the 0 into an error string.
/* Grow `list->owned_flags` to match `list->capacity`. Idempotent.
 * Returns 1 on success, 0 on alloc failure. Called lazily — the
 * flags array is only allocated when the first owned-put happens
 * on a list. Plain-only-element lists (literals, ints) never
 * allocate the flags array. */
static int list_grow_owned_flags(ArrayList* list) {
    if (!list || list->capacity == 0) return 1;
    if (list->owned_flags) return 1;
    list->owned_flags = (int*)cl_calloc(list->alloc,
        (size_t)list->capacity, sizeof(int));
    return list->owned_flags ? 1 : 0;
}

int list_add_raw(ArrayList* list, void* item) {
    if (!list) return 0;

    if (list->size >= list->capacity) {
        int new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        size_t old_bytes = (size_t)list->capacity * sizeof(void*);
        size_t new_bytes = (size_t)new_capacity * sizeof(void*);
        void** new_items = (void**)cl_realloc(list->alloc, list->items,
                                              old_bytes, new_bytes);
        if (!new_items) return 0;
        list->items = new_items;
        /* Mirror the grow in owned_flags when present. New slots
         * get value 0 (caller-owned default). */
        if (list->owned_flags) {
            size_t old_fbytes = (size_t)list->capacity * sizeof(int);
            size_t new_fbytes = (size_t)new_capacity * sizeof(int);
            int* new_flags = (int*)cl_realloc(list->alloc, list->owned_flags,
                                              old_fbytes, new_fbytes);
            if (!new_flags) return 0;
            /* Zero the new tail explicitly — aether_caps_realloc
             * doesn't zero-fill the extension. */
            memset((char*)new_flags + old_fbytes, 0, new_fbytes - old_fbytes);
            list->owned_flags = new_flags;
        }
        list->capacity = new_capacity;
    }

    list->items[list->size] = item;
    if (list->owned_flags) list->owned_flags[list->size] = 0;
    list->size++;
    return 1;
}

/* Heap-string-aware add (#467). Retains the AetherString so the
 * sender's reassign-wrapper / function-exit defer doesn't dangle
 * the list's reference, and tags this specific element as owned
 * so `list_free` releases it. Codegen routes `list.add(l, <heap_
 * string_expr>)` here when the static type of the value is
 * `string` and the expression is heap-classified.
 *
 * Per-element ownership lets a single list mix owned heap strings
 * with literals / ints. Each element's `owned_flags[i]` tracks
 * its own ownership independently. */
int list_add_string_owned(ArrayList* list, const void* item) {
    if (!list) return 0;
    /* Ownership transfer, not a share: the codegen routes here only when
     * `item` is a heap string escaping into the list (caller escape-marked,
     * does not release), so the list adopts the caller's single reference.
     * No string_retain — that would leave the value one refcount above what
     * list_free can reclaim (a per-add leak once values are magic strings).
     * The matching release is in list_free (owned_flags path). */
    /* Lazy-allocate the flags array on first owned-put. */
    if (!list->owned_flags) {
        if (list->capacity > 0) {
            list->owned_flags = (int*)cl_calloc(list->alloc,
                (size_t)list->capacity, sizeof(int));
            if (!list->owned_flags) {
                /* Best-effort: still store the item even if flag
                 * tracking failed; the buffer will leak on free
                 * rather than crash. */
            }
        }
    }
    int slot = list->size;  /* the index list_add_raw will use */
    int ok = list_add_raw(list, (void*)item);
    if (!ok) return 0;
    /* The first owned-put may have triggered the lazy alloc above
     * AFTER the items array existed but BEFORE list_add_raw's
     * grow path — flags array may still be NULL if list->capacity
     * was 0 at lazy-alloc time. Retry now that list_add_raw has
     * grown capacity. */
    if (!list->owned_flags && list->capacity > 0) {
        list->owned_flags = (int*)cl_calloc(list->alloc,
            (size_t)list->capacity, sizeof(int));
    }
    if (list->owned_flags) list->owned_flags[slot] = 1;
    return 1;
}

/* Layout-compatible view of the codegen's `_AeClosure` box (a
 * heap-allocated { fn, env } pair from _aether_box_closure). Used so
 * list_free can reclaim a boxed closure's captured environment as well
 * as the box itself. The field order/types mirror the prologue typedef
 * exactly; a function pointer and a data pointer are the same width on
 * every target we build for. */
typedef struct { void (*fn)(void); void* env; } AeClosureBox;

/* Add a heap-allocated closure BOX the list owns (owned_flags == 2).
 * Unlike a string value, the box also owns its captured `env`, so
 * list_free frees env first, then the box. Routed from codegen when a
 * `fn`-typed closure value is stored into a list (the fn -> ptr box
 * coercion). */
int list_add_closure_owned(ArrayList* list, void* box) {
    if (!list) return 0;
    if (!list->owned_flags && list->capacity > 0) {
        list->owned_flags = (int*)cl_calloc(list->alloc,
            (size_t)list->capacity, sizeof(int));
    }
    int slot = list->size;
    int ok = list_add_raw(list, box);
    if (!ok) return 0;
    if (!list->owned_flags && list->capacity > 0) {
        list->owned_flags = (int*)cl_calloc(list->alloc,
            (size_t)list->capacity, sizeof(int));
    }
    if (list->owned_flags) list->owned_flags[slot] = 2;
    return 1;
}

void* list_get_raw(ArrayList* list, int index) {
    /* Validity guard before touching any field. `list` may be NULL, a
     * low-address scalar (a small int intptr-cast to `ptr`), a freed list
     * (list_free zeroes `_kind_magic`), a struct whose memory has been
     * reused, or another kind (e.g. a HashMap) mistaken for a list. Any of
     * those would otherwise fault on `list->size` / `list->items[index]`.
     * Applying the same `_kind_magic` + low-address discriminator that
     * aether_value_is_list uses turns a type-confusion / use-after-free
     * into a safe NULL instead of a SIGSEGV deep inside the accessor. */
    if (!list || (uintptr_t)list < AETHER_KIND_MIN_VALID_ADDR
              || list->_kind_magic != AETHER_KIND_LIST_MAGIC) {
        return NULL;
    }
    if (index < 0 || index >= list->size) return NULL;
    return list->items[index];
}

void list_set(ArrayList* list, int index, void* item) {
    if (!list || index < 0 || index >= list->size) return;
    list->items[index] = item;
}

int list_size(ArrayList* list) {
    return list ? list->size : 0;
}

void list_remove(ArrayList* list, int index) {
    if (!list || index < 0 || index >= list->size) return;

    for (int i = index; i < list->size - 1; i++) {
        list->items[i] = list->items[i + 1];
    }
    list->size--;
}

void list_clear(ArrayList* list) {
    if (!list) return;
    list->size = 0;
}

void list_free(ArrayList* list) {
    if (!list) return;
    AetherAllocator* a = list->alloc;
    /* Owned heap-string elements (#467): walk and release each
     * `owned_flags[i] == 1` element before freeing the backing
     * array. Per-element tracking lets owned heap strings coexist
     * with literals / ints in the same list — only the owned
     * slots get released. AetherString (magic-tagged) goes through
     * string_release; plain heap char* goes through libc free. */
    if (list->owned_flags && list->items) {
        for (int i = 0; i < list->size; i++) {
            if (!list->owned_flags[i]) continue;
            void* it = list->items[i];
            if (!it) continue;
            if (list->owned_flags[i] == 2) {
                /* Owned closure box: reclaim the captured env, then the
                 * box. env is NULL for a non-capturing closure (no-op). */
                void* env = ((AeClosureBox*)it)->env;
                if (env) free(env);
                free(it);
            } else if (is_aether_string(it)) {
                string_release(it);
            } else {
                free(it);
            }
            list->items[i] = NULL;
        }
    }
    if (list->owned_flags) {
        cl_free(a, list->owned_flags,
                (size_t)list->capacity * sizeof(int));
        list->owned_flags = NULL;
    }
    /* Clear the kind-magic so a use-after-free probe via
     * aether_value_kind() returns AETHER_KIND_UNKNOWN rather than
     * false-matching the freed-but-still-readable memory. Defense
     * in depth — the host should not be using a freed pointer at
     * all, but we make the failure mode "kind says unknown,
     * deep-free skips" instead of "kind says list, deep-free
     * walks freed memory". */
    list->_kind_magic = 0;
    /* Items array's allocated bytes = capacity × sizeof(void*).
     * Struct is sizeof(ArrayList). Both pair with the alloc-side
     * accounting in list_new + list_add_raw. */
    if (list->items) {
        cl_free(a, list->items,
                (size_t)list->capacity * sizeof(void*));
    }
    cl_free(a, list, sizeof(ArrayList));
}

#define HASHMAP_INITIAL_CAPACITY 16
// Load factor = 3/4; expressed as an integer comparison below so we
// don't do FP arithmetic on every put.
#define HASHMAP_LOAD_NUMERATOR   3
#define HASHMAP_LOAD_DENOMINATOR 4

typedef struct HashMapEntry {
    AetherString*         key;
    void*                 value;
    struct HashMapEntry*  next;
    unsigned int          hash;    // cached so resize doesn't recompute
    unsigned int          key_len; // cached so key_equals can prefilter
    /* #467: per-entry heap-string-value ownership. Set to 1 by
     * `map_put_string_owned`; cleared by `map_put_raw` (or by
     * an owned-put that finds a prior unowned entry, etc.). At
     * map_clear / map_free / map_remove time, only entries with
     * value_owned == 1 have their value released — mixing owned
     * (heap concat results) and unowned (literal strings, int-
     * cast-to-ptr) values on the same map is safe by construction:
     * each entry knows who owns it. */
    int                   value_owned;
} HashMapEntry;

struct HashMap {
    uint32_t _kind_magic;       /* = AETHER_KIND_MAP_MAGIC */
    HashMapEntry** buckets;
    int capacity;
    int size;
};

// djb2. Also returns length via an out-param so callers don't walk the
// key twice (once to hash, once to strlen).
static unsigned int hash_cstr_len(const char* key, unsigned int* out_len) {
    unsigned int hash = 5381;
    const char* p = key;
    while (*p) {
        hash = ((hash << 5) + hash) + (unsigned char)*p;
        p++;
    }
    if (out_len) *out_len = (unsigned int)(p - key);
    return hash;
}

static unsigned int hash_cstr(const char* key) {
    return hash_cstr_len(key, NULL);
}

// Fast equality: cheap length compare first, memcmp only on match.
static int key_equals(const HashMapEntry* e, const char* b, unsigned int b_len) {
    if (!e || !e->key || !b) return 0;
    if (e->key_len != b_len) return 0;
    return memcmp(e->key->data, b, b_len) == 0;
}

HashMap* map_new() {
    HashMap* map = (HashMap*)aether_caps_malloc(sizeof(HashMap));
    if (!map) return NULL;
    map->_kind_magic = AETHER_KIND_MAP_MAGIC;
    map->capacity = HASHMAP_INITIAL_CAPACITY;
    map->size = 0;
    map->buckets = (HashMapEntry**)aether_caps_calloc(
        (size_t)map->capacity, sizeof(HashMapEntry*));
    if (!map->buckets) { aether_caps_free(map, sizeof(HashMap)); return NULL; }
    return map;
}

static void hashmap_resize(HashMap* map) {
    int old_capacity = map->capacity;
    HashMapEntry** old_buckets = map->buckets;
    int new_capacity = map->capacity * 2;

    HashMapEntry** new_buckets = (HashMapEntry**)aether_caps_calloc(
        (size_t)new_capacity, sizeof(HashMapEntry*));
    if (!new_buckets) return;  // Keep existing map on alloc failure

    map->capacity = new_capacity;
    map->buckets = new_buckets;
    // map->size stays the same — we're moving the same entries into
    // new buckets, not adding new ones.

    for (int i = 0; i < old_capacity; i++) {
        HashMapEntry* entry = old_buckets[i];
        while (entry) {
            HashMapEntry* next = entry->next;
            // Use cached hash; avoid walking the key string again.
            unsigned int index = entry->hash % (unsigned int)map->capacity;
            entry->next = map->buckets[index];
            map->buckets[index] = entry;
            entry = next;
        }
    }

    aether_caps_free(old_buckets,
                     (size_t)old_capacity * sizeof(HashMapEntry*));
}

// map_put_raw returns 1 on success, 0 on failure (null map/key or OOM).
// The Aether wrapper `map.put` in std.map/std.collections turns the 0
// into an error string.
int map_put_raw(HashMap* map, const char* key, void* value) {
    if (!map || !key) return 0;

    // Integer load-factor check: resize when size/capacity > 3/4.
    if ((long)map->size * HASHMAP_LOAD_DENOMINATOR >
        (long)map->capacity * HASHMAP_LOAD_NUMERATOR) {
        hashmap_resize(map);
    }

    unsigned int key_len = 0;
    unsigned int hash = hash_cstr_len(key, &key_len);
    unsigned int index = hash % (unsigned int)map->capacity;
    HashMapEntry* entry = map->buckets[index];

    while (entry) {
        // Hash check is a cheap prefilter before key_equals (which
        // still runs memcmp for false hash collisions).
        if (entry->hash == hash && key_equals(entry, key, key_len)) {
            /* #467: if the entry was previously owned-put, release
             * the prior heap-string before overwriting with the
             * (unowned) new value. Subsequent map.free won't see
             * the dropped allocation since value_owned is reset. */
            if (entry->value_owned && entry->value) {
                if (is_aether_string(entry->value)) {
                    string_release(entry->value);
                } else {
                    free(entry->value);
                }
            }
            entry->value = value;
            entry->value_owned = 0;
            return 1;
        }
        entry = entry->next;
    }

    HashMapEntry* new_entry = (HashMapEntry*)aether_caps_malloc(sizeof(HashMapEntry));
    if (!new_entry) return 0;
    new_entry->key = string_new(key);
    if (!new_entry->key) { aether_caps_free(new_entry, sizeof(HashMapEntry)); return 0; }
    new_entry->value       = value;
    new_entry->value_owned = 0;
    new_entry->hash        = hash;
    new_entry->key_len     = key_len;
    new_entry->next        = map->buckets[index];
    map->buckets[index] = new_entry;
    map->size++;
    return 1;
}

/* Heap-string-aware put (#467). Mirrors list_add_string_owned for
 * map values: retains the value (AetherString refcount bump),
 * tags the map as owning string values, then stores via the plain
 * map_put_raw path. map_clear / map_free walk owned-string maps
 * and release each value before freeing the bucket entries.
 *
 * The key is always owned by the map (string_new'd at put time,
 * released at clear time) — unchanged from map_put_raw. The
 * value is what this variant adds heap-string tracking for.
 *
 * Codegen routes `map.put(m, k, heap_string_expr)` here when the
 * value is heap-classified (string.concat, string interp, heap-
 * returning user-fn). Plain literals stay on map_put_raw. */
int map_put_string_owned(HashMap* map, const char* key, const void* value) {
    if (!map || !key) return 0;
    /* The codegen routes here only when `value` is a heap string that
     * ESCAPES into the map — the caller is escape-marked and does NOT
     * release its reference. So the map adopts the caller's single
     * reference (ownership transfer); it must NOT string_retain, or the
     * value would carry one refcount more than the map_free release can
     * reclaim. Before the magic-string unification this retain was a
     * silent no-op (values were plain char*); a magic value now makes
     * the imbalance a per-put leak. The matching release lives in
     * map_free / map_clear / the replace path below. */

    /* Replace path: walk the bucket. If the key exists, release
     * the previous value when it was previously owned, then store
     * the new value with value_owned=1. If the previous put was
     * unowned (literal / int-cast-to-ptr), DON'T release it —
     * the caller never expected ownership transfer for that path. */
    if (map->size > 0) {
        unsigned int key_len = 0;
        unsigned int hash = hash_cstr_len(key, &key_len);
        unsigned int index = hash % (unsigned int)map->capacity;
        HashMapEntry* entry = map->buckets[index];
        while (entry) {
            if (entry->hash == hash && key_equals(entry, key, key_len)) {
                if (entry->value_owned && entry->value) {
                    if (is_aether_string(entry->value)) {
                        string_release(entry->value);
                    } else {
                        free(entry->value);
                    }
                }
                entry->value = (void*)value;
                entry->value_owned = 1;
                return 1;
            }
            entry = entry->next;
        }
    }

    /* Fresh entry: insert via map_put_raw, then flip the new
     * entry's value_owned. map_put_raw guarantees the inserted
     * entry is at the head of the bucket. */
    int ok = map_put_raw(map, key, (void*)value);
    if (!ok) return 0;
    unsigned int key_len = 0;
    unsigned int hash = hash_cstr_len(key, &key_len);
    unsigned int index = hash % (unsigned int)map->capacity;
    HashMapEntry* head = map->buckets[index];
    if (head) head->value_owned = 1;
    return 1;
}

void* map_get_raw(HashMap* map, const char* key) {
    if (!map || !key) return NULL;

    unsigned int key_len = 0;
    unsigned int hash = hash_cstr_len(key, &key_len);
    unsigned int index = hash % (unsigned int)map->capacity;
    HashMapEntry* entry = map->buckets[index];

    while (entry) {
        if (entry->hash == hash && key_equals(entry, key, key_len)) {
            return entry->value;
        }
        entry = entry->next;
    }

    return NULL;
}

int map_has(HashMap* map, const char* key) {
    return map_get_raw(map, key) != NULL;
}

void map_remove(HashMap* map, const char* key) {
    if (!map || !key) return;

    unsigned int key_len = 0;
    unsigned int hash = hash_cstr_len(key, &key_len);
    unsigned int index = hash % (unsigned int)map->capacity;
    HashMapEntry* entry = map->buckets[index];
    HashMapEntry* prev = NULL;

    while (entry) {
        if (entry->hash == hash && key_equals(entry, key, key_len)) {
            if (prev) {
                prev->next = entry->next;
            } else {
                map->buckets[index] = entry->next;
            }

            string_release(entry->key);
            /* #467: release the value too when this entry was
             * owned-put. Otherwise map_remove of an owned-put
             * entry leaks the heap string. */
            if (entry->value_owned && entry->value) {
                if (is_aether_string(entry->value)) {
                    string_release(entry->value);
                } else {
                    free(entry->value);
                }
            }
            aether_caps_free(entry, sizeof(HashMapEntry));
            map->size--;
            return;
        }
        prev = entry;
        entry = entry->next;
    }
}

int map_size(HashMap* map) {
    return map ? map->size : 0;
}

void map_clear(HashMap* map) {
    if (!map) return;

    for (int i = 0; i < map->capacity; i++) {
        HashMapEntry* entry = map->buckets[i];
        while (entry) {
            HashMapEntry* next = entry->next;
            string_release(entry->key);
            /* #467: release the value only when this specific
             * entry was owned-put. Per-entry tracking lets a
             * single map carry owned heap-strings + unowned
             * literals / int-cast-to-ptr values without
             * crashing on free of the latter. */
            if (entry->value_owned && entry->value) {
                if (is_aether_string(entry->value)) {
                    string_release(entry->value);
                } else {
                    free(entry->value);
                }
            }
            aether_caps_free(entry, sizeof(HashMapEntry));
            entry = next;
        }
        map->buckets[i] = NULL;
    }
    map->size = 0;
}

void map_free(HashMap* map) {
    if (!map) return;
    map_clear(map);
    /* Same defense-in-depth as list_free: clear the magic so a UAF
     * probe sees AETHER_KIND_UNKNOWN. */
    map->_kind_magic = 0;
    aether_caps_free(map->buckets,
                     (size_t)map->capacity * sizeof(HashMapEntry*));
    aether_caps_free(map, sizeof(HashMap));
}

MapKeys* map_keys_raw(HashMap* map) {
    if (!map) return NULL;

    MapKeys* keys = (MapKeys*)aether_caps_malloc(sizeof(MapKeys));
    if (!keys) return NULL;
    keys->count = 0;
    if (map->size == 0) {
        keys->keys = NULL;
        return keys;
    }
    /* The snapshot array holds exactly map->size pointers; the fill loop
     * below increments keys->count once per entry, so at free time
     * keys->count == map->size and the matching aether_caps_free size is
     * exact regardless of later map mutation. */
    keys->keys = (AetherString**)aether_caps_malloc(map->size * sizeof(AetherString*));
    if (!keys->keys) { aether_caps_free(keys, sizeof(MapKeys)); return NULL; }

    for (int i = 0; i < map->capacity; i++) {
        HashMapEntry* entry = map->buckets[i];
        while (entry) {
            keys->keys[keys->count++] = entry->key;
            entry = entry->next;
        }
    }

    return keys;
}

void map_keys_free(MapKeys* keys) {
    if (!keys) return;
    aether_caps_free(keys->keys, (size_t)keys->count * sizeof(AetherString*));
    aether_caps_free(keys, sizeof(MapKeys));
}

