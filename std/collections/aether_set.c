#include "aether_set.h"

/* A set is a hash map whose values carry no information. Aliasing the
 * map rather than wrapping it keeps one hash-table implementation in
 * the tree: key copying, resizing and cap accounting all stay in
 * aether_collections.c. */
static HashMap* as_map(AetherSet* set) {
    return (HashMap*)set;
}

/* Stored as every item's value purely so entries hold a non-null
 * pointer. Its address is never dereferenced and its contents are
 * never read. */
static char set_present;

AetherSet* aether_set_new(void) {
    return (AetherSet*)map_new();
}

int aether_set_add(AetherSet* set, const char* item) {
    if (!set || !item) return -1;
    if (map_has(as_map(set), item)) return 0;
    return map_put_raw(as_map(set), item, &set_present) ? 1 : -1;
}

int aether_set_has(AetherSet* set, const char* item) {
    if (!set || !item) return 0;
    return map_has(as_map(set), item);
}

void aether_set_remove(AetherSet* set, const char* item) {
    if (!set || !item) return;
    map_remove(as_map(set), item);
}

int aether_set_size(AetherSet* set) {
    if (!set) return 0;
    return map_size(as_map(set));
}

void aether_set_clear(AetherSet* set) {
    if (!set) return;
    map_clear(as_map(set));
}

void aether_set_free(AetherSet* set) {
    if (!set) return;
    map_free(as_map(set));
}

MapKeys* aether_set_items_raw(AetherSet* set) {
    if (!set) return NULL;
    return map_keys_raw(as_map(set));
}

void aether_set_items_free(MapKeys* items) {
    map_keys_free(items);
}
