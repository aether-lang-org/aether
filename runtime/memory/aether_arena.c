#include "aether_arena.h"
#include "../aether_resource_caps.h"
#include <stdlib.h>
#include <string.h>

#define ARENA_DEFAULT_SIZE (1024 * 1024)
#define ALIGN_UP(n, align) (((n) + (align) - 1) & ~((align) - 1))

Arena* arena_create(size_t size) {
    if (size == 0) {
        size = ARENA_DEFAULT_SIZE;
    }

    /* #343: cap-aware. Both the Arena struct and the backing memory
     * block are accounted; arena_destroy frees them with their
     * known sizes (sizeof(Arena) and arena->size). */
    Arena* arena = (Arena*)aether_caps_malloc(sizeof(Arena));
    if (!arena) return NULL;

    arena->memory = (char*)aether_caps_malloc(size);
    if (!arena->memory) {
        aether_caps_free(arena, sizeof(Arena));
        return NULL;
    }

    arena->size = size;
    arena->used = 0;
    arena->next = NULL;

    return arena;
}

void* arena_alloc(Arena* arena, size_t bytes) {
    return arena_alloc_aligned(arena, bytes, 8);
}

void* arena_alloc_aligned(Arena* arena, size_t bytes, size_t alignment) {
    if (!arena || bytes == 0) return NULL;
    
    size_t aligned_used = ALIGN_UP(arena->used, alignment);
    
    if (aligned_used + bytes > arena->size) {
        if (!arena->next) {
            size_t new_size = arena->size;
            if (bytes > new_size) {
                new_size = ALIGN_UP(bytes, ARENA_DEFAULT_SIZE);
            }
            arena->next = arena_create(new_size);
        }
        return arena_alloc_aligned(arena->next, bytes, alignment);
    }
    
    void* ptr = arena->memory + aligned_used;
    arena->used = aligned_used + bytes;
    
    return ptr;
}

void arena_reset(Arena* arena) {
    if (!arena) return;
    
    arena->used = 0;
    
    if (arena->next) {
        arena_destroy(arena->next);
        arena->next = NULL;
    }
}

void arena_destroy(Arena* arena) {
    while (arena) {
        Arena* next = arena->next;
        aether_caps_free(arena->memory, arena->size);
        aether_caps_free(arena, sizeof(Arena));
        arena = next;
    }
}

ArenaScope arena_begin(Arena* arena) {
    ArenaScope scope;
    scope.arena = arena;
    scope.restore_point = arena ? arena->used : 0;
    return scope;
}

void arena_end(ArenaScope scope) {
    if (scope.arena) {
        scope.arena->used = scope.restore_point;
    }
}

size_t arena_get_used(Arena* arena) {
    if (!arena) return 0;
    
    size_t total = arena->used;
    Arena* current = arena->next;
    while (current) {
        total += current->used;
        current = current->next;
    }
    
    return total;
}

size_t arena_get_size(Arena* arena) {
    if (!arena) return 0;
    
    size_t total = arena->size;
    Arena* current = arena->next;
    while (current) {
        total += current->size;
        current = current->next;
    }
    
    return total;
}


/* The std.alloc arena bridge lives here, not in std/alloc/aether_alloc.c:
 * it references the arena, which the bootstrap compiler does not link while
 * it does link std/alloc via STD_SRC. Moving it there breaks the compiler
 * link. */
#include "../../std/alloc/aether_alloc.h"

static void* arena_alloc_fn(void* data, size_t size) {
    return arena_alloc((Arena*)data, size);
}
static void* arena_realloc_fn(void* data, void* ptr, size_t old_size, size_t new_size) {
    if (new_size == 0) return NULL;
    void* dst = arena_alloc((Arena*)data, new_size);
    if (dst && ptr) {
        size_t n = old_size < new_size ? old_size : new_size;
        memcpy(dst, ptr, n);
    }
    return dst;
}
static void arena_free_fn(void* data, void* ptr, size_t size) {
    (void)data; (void)ptr; (void)size;
}

AetherAllocator* aether_allocator_arena(struct Arena* arena) {
    if (!arena) return NULL;
    AetherAllocator* a = (AetherAllocator*)malloc(sizeof(AetherAllocator));
    if (!a) return NULL;
    a->alloc_fn   = arena_alloc_fn;
    a->realloc_fn = arena_realloc_fn;
    a->free_fn    = arena_free_fn;
    a->data       = arena;
    return a;
}

void aether_allocator_arena_destroy(AetherAllocator* a) {
    free(a);
}
