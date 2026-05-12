#include "aether_collections.h"
#include "../../runtime/aether_resource_caps.h"
#include "../../runtime/aether_value_kind.h"
#include "../string/aether_string.h"
#include <stdlib.h>
#include <string.h>

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
    int owns_string_elements;
};

ArrayList* list_new() {
    /* #343: cap-aware. Items array is allocated lazily on first
     * add — only the struct is accounted here. */
    ArrayList* list = (ArrayList*)aether_caps_malloc(sizeof(ArrayList));
    if (!list) return NULL;
    list->_kind_magic = AETHER_KIND_LIST_MAGIC;
    list->items = NULL;
    list->size = 0;
    list->capacity = 0;
    list->owns_string_elements = 0;
    return list;
}

// list_add_raw returns 1 on success, 0 on failure (realloc error or
// null list). The Aether wrapper `list.add` in std.list/std.collections
// turns the 0 into an error string.
int list_add_raw(ArrayList* list, void* item) {
    if (!list) return 0;

    if (list->size >= list->capacity) {
        int new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        size_t old_bytes = (size_t)list->capacity * sizeof(void*);
        size_t new_bytes = (size_t)new_capacity * sizeof(void*);
        void** new_items = (void**)aether_caps_realloc(list->items,
                                                       old_bytes, new_bytes);
        if (!new_items) return 0;
        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->size++] = item;
    return 1;
}

/* Heap-string-aware add (#467). Retains the AetherString so the
 * sender's reassign-wrapper / function-exit defer doesn't dangle
 * the list's reference, and tags the list as owning its string
 * elements so `list_free` releases them. Codegen routes
 * `list.add(l, <heap_string_expr>)` here when the static type of
 * the value is `string` and the expression is heap-classified
 * (string.concat, _aether_interp, heap-returning user fn, etc.).
 *
 * For literal-string adds (`list.add(l, "literal")`), codegen
 * stays on the plain `list_add_raw` path — literals don't need
 * retain and the list shouldn't release them at free time.
 *
 * Mixed lists (one heap-string add followed by a literal add or a
 * plain int add) aren't covered by this flag — Aether's type
 * system pins each list to a single element type at the call site,
 * so codegen can pick the right path consistently. */
int list_add_string_owned(ArrayList* list, const void* item) {
    if (!list) return 0;
    /* Retain on add — if the source local is later reassigned or
     * goes out of scope, the list's reference keeps the buffer
     * alive. No-op on plain (non-AetherString) char* pointers. */
    if (item) string_retain(item);
    list->owns_string_elements = 1;
    return list_add_raw(list, (void*)item);
}

void* list_get_raw(ArrayList* list, int index) {
    if (!list || index < 0 || index >= list->size) return NULL;
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
    /* Owned heap-string elements (#467): walk and release each
     * before freeing the backing array. Two element shapes:
     *
     *   - AetherString* (magic-tagged): `string_release` decrements
     *     refcount, frees struct + data at zero.
     *   - plain `char*` heap (e.g. from `string_concat`): plain
     *     `free()` reclaims it.
     *
     * `is_aether_string` (byte-by-byte magic probe, ASan-clean on
     * short literals) discriminates. Plain pointers from
     * `string_concat` etc. don't carry the magic header; AetherStrings
     * from `string_new_with_length` etc. do. */
    if (list->owns_string_elements && list->items) {
        for (int i = 0; i < list->size; i++) {
            void* it = list->items[i];
            if (!it) continue;
            if (is_aether_string(it)) {
                string_release(it);
            } else {
                free(it);
            }
            list->items[i] = NULL;
        }
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
        aether_caps_free(list->items,
                         (size_t)list->capacity * sizeof(void*));
    }
    aether_caps_free(list, sizeof(ArrayList));
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
} HashMapEntry;

struct HashMap {
    uint32_t _kind_magic;       /* = AETHER_KIND_MAP_MAGIC */
    HashMapEntry** buckets;
    int capacity;
    int size;
    /* #467: set by `map_put_string_owned` when the put value is a
     * heap-string. map_clear walks entries and releases each
     * value (in addition to releasing the key, which it always
     * does). Same semantics as ArrayList's `owns_string_elements`. */
    int owns_string_values;
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
    map->owns_string_values = 0;
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
            entry->value = value;
            return 1;
        }
        entry = entry->next;
    }

    HashMapEntry* new_entry = (HashMapEntry*)aether_caps_malloc(sizeof(HashMapEntry));
    if (!new_entry) return 0;
    new_entry->key = string_new(key);
    if (!new_entry->key) { aether_caps_free(new_entry, sizeof(HashMapEntry)); return 0; }
    new_entry->value   = value;
    new_entry->hash    = hash;
    new_entry->key_len = key_len;
    new_entry->next    = map->buckets[index];
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
    if (!map) return 0;
    if (value) string_retain(value);
    map->owns_string_values = 1;
    /* If the put REPLACES an existing entry, the previous value
     * needs releasing too — otherwise replacement leaks the old
     * heap string. Walk to find any existing entry. */
    if (value || map->size > 0) {
        unsigned int key_len = 0;
        unsigned int hash = hash_cstr_len(key, &key_len);
        unsigned int index = hash % (unsigned int)map->capacity;
        HashMapEntry* entry = map->buckets[index];
        while (entry) {
            if (entry->hash == hash && key_equals(entry, key, key_len)) {
                if (entry->value) {
                    if (is_aether_string(entry->value)) {
                        string_release(entry->value);
                    } else {
                        free(entry->value);
                    }
                }
                entry->value = (void*)value;
                return 1;
            }
            entry = entry->next;
        }
    }
    return map_put_raw(map, key, (void*)value);
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
            /* #467: release heap-string values too when the map
             * is in owns_string_values mode. Same shape as
             * list_free's element walk. */
            if (map->owns_string_values && entry->value) {
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

    MapKeys* keys = (MapKeys*)malloc(sizeof(MapKeys));
    if (!keys) return NULL;
    keys->count = 0;
    if (map->size == 0) {
        keys->keys = NULL;
        return keys;
    }
    keys->keys = (AetherString**)malloc(map->size * sizeof(AetherString*));
    if (!keys->keys) { free(keys); return NULL; }

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
    free(keys->keys);
    free(keys);
}

