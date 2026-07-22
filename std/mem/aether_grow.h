#ifndef AETHER_GROW_H
#define AETHER_GROW_H

#include <stddef.h>
#include <stdint.h>

// Capacity growth for the doubling buffers used across the stdlib
// (string builders, serializer scratch buffers, byte buffers).
//
// The naive form of this loop,
//
//     while (cap < need) cap *= 2;
//
// has two failure modes on large inputs: `cap * 2` can wrap to a value
// below `need` and spin forever, and the surviving capacity can wrap to
// something smaller than the caller asked for, which turns the following
// memcpy into a heap overflow. Both are avoided here.
//
// Callers keep their own allocator and error convention; only the size
// arithmetic is shared. Returns 0 when no valid capacity exists, which
// callers treat as an allocation failure.

// Smallest doubling capacity >= `need`, starting from `current` (or
// `initial` when `current` is 0). `elem_size` is the size of one slot,
// used to reject capacities whose byte count would overflow size_t.
// Returns 0 if `need` cannot be represented.
static inline size_t aether_buf_grow_capacity(size_t current,
                                              size_t initial,
                                              size_t need,
                                              size_t elem_size) {
    if (elem_size == 0) return 0;
    if (need > SIZE_MAX / elem_size) return 0;

    size_t cap = current ? current : (initial ? initial : 1);
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            // Doubling would wrap: fall back to the exact request.
            cap = need;
            break;
        }
        cap *= 2;
    }
    if (cap > SIZE_MAX / elem_size) return 0;
    return cap;
}

#endif // AETHER_GROW_H
