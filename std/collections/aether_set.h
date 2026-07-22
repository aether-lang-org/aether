#ifndef AETHER_SET_H
#define AETHER_SET_H

#include "aether_collections.h"

// Unordered set of unique strings, backed by the std.map hash table.
// Keys are copied on insert, so the caller's string lifetime does not
// matter. Aether-facing wrappers live in std/set/module.ae.

typedef struct AetherSet AetherSet;

// Returns NULL on allocation failure.
AetherSet* aether_set_new(void);

// 1 if the item was added, 0 if it was already present, -1 on a null
// argument or allocation failure.
int aether_set_add(AetherSet* set, const char* item);

// 1 if present, 0 otherwise (also 0 for a null set or item).
int aether_set_has(AetherSet* set, const char* item);

// No-op when the item is absent.
void aether_set_remove(AetherSet* set, const char* item);

int aether_set_size(AetherSet* set);

// Drops every item but keeps the set usable.
void aether_set_clear(AetherSet* set);

void aether_set_free(AetherSet* set);

// Snapshot of the set's items in unspecified order. Caller frees with
// aether_set_items_free. Returns NULL on allocation failure or a null set.
MapKeys* aether_set_items_raw(AetherSet* set);
void aether_set_items_free(MapKeys* items);

#endif // AETHER_SET_H
