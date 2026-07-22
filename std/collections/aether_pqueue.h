#ifndef AETHER_PQUEUE_H
#define AETHER_PQUEUE_H

// Priority queue over (priority, item) pairs, backed by a binary heap.
// Lowest priority value comes out first; negate the priority for
// highest-first. Push and pop are O(log n), peek and size are O(1).
//
// The queue stores item pointers without taking ownership: it never
// frees them, so the caller decides their lifetime. Aether-facing
// wrappers live in std/pqueue/module.ae.

typedef struct AetherPQueue AetherPQueue;

// Returns NULL on allocation failure.
AetherPQueue* aether_pqueue_new(void);

// 1 on success, 0 on a null queue or allocation failure. Priority is
// long long, not long: Aether `long` is 64-bit and Windows C `long`
// is 32, matching the string_to_long_raw convention.
int aether_pqueue_push(AetherPQueue* pq, long long priority, void* item);

// Removes and returns the lowest-priority item, or NULL when empty.
// A NULL return is ambiguous if NULL items were pushed; use
// aether_pqueue_size to distinguish.
void* aether_pqueue_pop(AetherPQueue* pq);

// Lowest-priority item without removing it, or NULL when empty.
void* aether_pqueue_peek(AetherPQueue* pq);

// Priority of the item aether_pqueue_peek would return. Returns 0 when empty,
// so check aether_pqueue_size first.
long long aether_pqueue_peek_priority(AetherPQueue* pq);

int aether_pqueue_size(AetherPQueue* pq);
int aether_pqueue_is_empty(AetherPQueue* pq);

// Drops every entry but keeps the queue usable. Items are not freed.
void aether_pqueue_clear(AetherPQueue* pq);

void aether_pqueue_free(AetherPQueue* pq);

#endif // AETHER_PQUEUE_H
