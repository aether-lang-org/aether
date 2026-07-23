#include "aether_pqueue.h"
#include "../../runtime/aether_resource_caps.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#define PQUEUE_INITIAL_CAPACITY 16

typedef struct {
    long long priority;
    void* item;
} PQueueEntry;

struct AetherPQueue {
    PQueueEntry* entries;
    int          size;
    int          capacity;
};

static size_t entries_bytes(int capacity) {
    return (size_t)capacity * sizeof(PQueueEntry);
}

static int pqueue_grow(AetherPQueue* pq) {
    /* CRITICAL: capacity is an int scaled by sizeof(PQueueEntry). Doubling
     * unchecked would overflow into a small allocation and let the heap
     * writes below run past the buffer. */
    if (pq->capacity > INT_MAX / 2) return 0;
    int new_capacity = pq->capacity ? pq->capacity * 2 : PQUEUE_INITIAL_CAPACITY;
    if ((size_t)new_capacity > SIZE_MAX / sizeof(PQueueEntry)) return 0;

    PQueueEntry* grown = (PQueueEntry*)aether_caps_realloc(
        pq->entries, entries_bytes(pq->capacity), entries_bytes(new_capacity));
    if (!grown) return 0;

    pq->entries  = grown;
    pq->capacity = new_capacity;
    return 1;
}

static void swap_entries(PQueueEntry* a, PQueueEntry* b) {
    PQueueEntry tmp = *a;
    *a = *b;
    *b = tmp;
}

static void sift_up(AetherPQueue* pq, int index) {
    while (index > 0) {
        int parent = (index - 1) / 2;
        if (pq->entries[parent].priority <= pq->entries[index].priority) break;
        swap_entries(&pq->entries[parent], &pq->entries[index]);
        index = parent;
    }
}

static void sift_down(AetherPQueue* pq, int index) {
    for (;;) {
        int left     = index * 2 + 1;
        int right    = left + 1;
        int smallest = index;

        if (left < pq->size &&
            pq->entries[left].priority < pq->entries[smallest].priority) {
            smallest = left;
        }
        if (right < pq->size &&
            pq->entries[right].priority < pq->entries[smallest].priority) {
            smallest = right;
        }
        if (smallest == index) break;

        swap_entries(&pq->entries[smallest], &pq->entries[index]);
        index = smallest;
    }
}

AetherPQueue* aether_pqueue_new(void) {
    AetherPQueue* pq = (AetherPQueue*)aether_caps_calloc(1, sizeof(AetherPQueue));
    return pq;
}

int aether_pqueue_push(AetherPQueue* pq, long long priority, void* item) {
    if (!pq) return 0;
    if (pq->size == pq->capacity && !pqueue_grow(pq)) return 0;

    pq->entries[pq->size].priority = priority;
    pq->entries[pq->size].item     = item;
    pq->size++;
    sift_up(pq, pq->size - 1);
    return 1;
}

void* aether_pqueue_pop(AetherPQueue* pq) {
    if (!pq || pq->size == 0) return NULL;

    void* item = pq->entries[0].item;
    pq->size--;
    if (pq->size > 0) {
        pq->entries[0] = pq->entries[pq->size];
        sift_down(pq, 0);
    }
    return item;
}

void* aether_pqueue_peek(AetherPQueue* pq) {
    if (!pq || pq->size == 0) return NULL;
    return pq->entries[0].item;
}

long long aether_pqueue_peek_priority(AetherPQueue* pq) {
    if (!pq || pq->size == 0) return 0;
    return pq->entries[0].priority;
}

int aether_pqueue_size(AetherPQueue* pq) {
    return pq ? pq->size : 0;
}

int aether_pqueue_is_empty(AetherPQueue* pq) {
    return (!pq || pq->size == 0) ? 1 : 0;
}

void aether_pqueue_clear(AetherPQueue* pq) {
    if (!pq) return;
    pq->size = 0;
}

void aether_pqueue_free(AetherPQueue* pq) {
    if (!pq) return;
    if (pq->entries) aether_caps_free(pq->entries, entries_bytes(pq->capacity));
    aether_caps_free(pq, sizeof(AetherPQueue));
}
