#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../utils/aether_thread.h"
#include "../aether_runtime.h"

// Memory pool for efficient allocation
typedef struct MemoryPool {
    void* memory;
    size_t size;
    size_t used;
    struct MemoryPool* next;
} MemoryPool;

typedef struct {
    MemoryPool* pools;
    pthread_mutex_t mutex;
    size_t pool_size;
} MemoryManager;

static MemoryManager* g_memory_manager = NULL;

// Initialize memory manager
int aether_memory_init(size_t initial_pool_size) {
    if (g_memory_manager) return AETHER_SUCCESS;
    
    g_memory_manager = malloc(sizeof(MemoryManager));
    if (!g_memory_manager) return AETHER_ERROR_OUT_OF_MEMORY;
    
    g_memory_manager->pool_size = initial_pool_size > 0 ? initial_pool_size : 1024 * 1024; // 1MB default
    g_memory_manager->pools = NULL;
    
    if (pthread_mutex_init(&g_memory_manager->mutex, NULL) != 0) {
        free(g_memory_manager);
        g_memory_manager = NULL;
        return AETHER_ERROR_OUT_OF_MEMORY;
    }
    
    return AETHER_SUCCESS;
}

// Cleanup memory manager
void aether_memory_cleanup(void) {
    if (!g_memory_manager) return;
    
    pthread_mutex_lock(&g_memory_manager->mutex);
    
    MemoryPool* current = g_memory_manager->pools;
    while (current) {
        MemoryPool* next = current->next;
        free(current->memory);
        free(current);
        current = next;
    }
    
    pthread_mutex_unlock(&g_memory_manager->mutex);
    pthread_mutex_destroy(&g_memory_manager->mutex);
    free(g_memory_manager);
    g_memory_manager = NULL;
}

// Allocate memory from pool
void* aether_alloc(size_t size) {
    if (!g_memory_manager) {
        // Fallback to system malloc
        return malloc(size);
    }
    
    pthread_mutex_lock(&g_memory_manager->mutex);
    
    // Find a pool with enough space
    MemoryPool* pool = g_memory_manager->pools;
    while (pool) {
        if (pool->used + size <= pool->size) {
            void* ptr = (char*)pool->memory + pool->used;
            pool->used += size;
            pthread_mutex_unlock(&g_memory_manager->mutex);
            return ptr;
        }
        pool = pool->next;
    }
    
    // No pool has enough space, create new one
    MemoryPool* new_pool = malloc(sizeof(MemoryPool));
    if (!new_pool) {
        pthread_mutex_unlock(&g_memory_manager->mutex);
        return malloc(size); // Fallback
    }
    
    new_pool->size = g_memory_manager->pool_size;
    if (size > new_pool->size) {
        new_pool->size = size * 2; // Allocate extra space
    }
    
    new_pool->memory = malloc(new_pool->size);
    if (!new_pool->memory) {
        free(new_pool);
        pthread_mutex_unlock(&g_memory_manager->mutex);
        return malloc(size); // Fallback
    }
    
    new_pool->used = size;
    new_pool->next = g_memory_manager->pools;
    g_memory_manager->pools = new_pool;
    
    void* ptr = new_pool->memory;
    pthread_mutex_unlock(&g_memory_manager->mutex);
    
    return ptr;
}

// Free memory (for compatibility, but pools don't support individual free)
void aether_free(void* ptr) {
    // Memory pools don't support individual free
    // In a real implementation, you'd need a more sophisticated allocator
    // For now, we'll just use system malloc/free
    free(ptr);
}

// Allocate and zero memory
void* aether_calloc(size_t count, size_t size) {
    void* ptr = aether_alloc(count * size);
    if (ptr) {
        memset(ptr, 0, count * size);
    }
    return ptr;
}

// Reallocate memory
void* aether_realloc(void* ptr, size_t new_size) {
    // Memory pools don't support realloc efficiently
    // Fallback to system realloc
    return realloc(ptr, new_size);
}

// Duplicate string
char* aether_strdup(const char* str) {
    if (!str) return NULL;
    
    size_t len = strlen(str) + 1;
    char* copy = aether_alloc(len);
    if (copy) {
        memcpy(copy, str, len);
    }
    return copy;
}

// Memory statistics
typedef struct {
    size_t total_allocated;
    size_t total_used;
    int pool_count;
} MemoryStats;

MemoryStats aether_get_memory_stats(void) {
    MemoryStats stats = {0};
    
    if (!g_memory_manager) return stats;
    
    pthread_mutex_lock(&g_memory_manager->mutex);
    
    MemoryPool* current = g_memory_manager->pools;
    while (current) {
        stats.total_allocated += current->size;
        stats.total_used += current->used;
        stats.pool_count++;
        current = current->next;
    }
    
    pthread_mutex_unlock(&g_memory_manager->mutex);
    
    return stats;
}

void aether_print_memory_stats(void) {
    MemoryStats stats = aether_get_memory_stats();
    
    printf("\n=== Aether Memory Statistics ===\n");
    printf("Total allocated: %zu bytes (%.2f MB)\n", 
           stats.total_allocated, stats.total_allocated / (1024.0 * 1024.0));
    printf("Total used: %zu bytes (%.2f MB)\n", 
           stats.total_used, stats.total_used / (1024.0 * 1024.0));
    printf("Memory pools: %d\n", stats.pool_count);
    printf("================================\n\n");
}
