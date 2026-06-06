#include "aether_vector.h"
#include "../../runtime/aether_resource_caps.h"
#include <stdlib.h>
#include <string.h>

#define DEFAULT_CAPACITY 16
#define GROWTH_FACTOR 2

Vector* vector_create(size_t initial_capacity,
                     void (*element_free)(void*),
                     void* (*element_clone)(const void*)) {
    
    /* #463: cap-aware. Backing array byte count recoverable from
     * `vec->capacity * sizeof(void*)` at free / realloc. */
    Vector* vec = (Vector*)aether_caps_malloc(sizeof(Vector));
    if (!vec) return NULL;

    if (initial_capacity < DEFAULT_CAPACITY) {
        initial_capacity = DEFAULT_CAPACITY;
    }

    vec->data = (void**)aether_caps_malloc(initial_capacity * sizeof(void*));
    if (!vec->data) {
        aether_caps_free(vec, sizeof(Vector));
        return NULL;
    }
    
    vec->size = 0;
    vec->capacity = initial_capacity;
    vec->element_free = element_free;
    vec->element_clone = element_clone;
    
    return vec;
}

void vector_free(Vector* vec) {
    if (!vec) return;
    
    if (vec->data) {
        if (vec->element_free) {
            for (size_t i = 0; i < vec->size; i++) {
                vec->element_free(vec->data[i]);
            }
        }
        aether_caps_free(vec->data, vec->capacity * sizeof(void*));
    }

    aether_caps_free(vec, sizeof(Vector));
}

// Ensure capacity
static bool vector_ensure_capacity(Vector* vec, size_t min_capacity) {
    if (vec->capacity >= min_capacity) {
        return true;
    }
    
    size_t new_capacity = vec->capacity * GROWTH_FACTOR;
    if (new_capacity < min_capacity) {
        new_capacity = min_capacity;
    }
    
    void** new_data = (void**)aether_caps_realloc(vec->data,
                                                  vec->capacity * sizeof(void*),
                                                  new_capacity * sizeof(void*));
    if (!new_data) {
        return false;
    }

    vec->data = new_data;
    vec->capacity = new_capacity;
    return true;
}

bool vector_push(Vector* vec, void* element) {
    if (!vec) return false;
    
    if (!vector_ensure_capacity(vec, vec->size + 1)) {
        return false;
    }
    
    vec->data[vec->size++] = element;
    return true;
}

void* vector_pop(Vector* vec) {
    if (!vec || vec->size == 0) return NULL;
    return vec->data[--vec->size];
}

void* vector_get(Vector* vec, size_t index) {
    if (!vec || index >= vec->size) return NULL;
    return vec->data[index];
}

bool vector_set(Vector* vec, size_t index, void* element) {
    if (!vec || index >= vec->size) return false;
    
    if (vec->element_free) {
        vec->element_free(vec->data[index]);
    }
    
    vec->data[index] = element;
    return true;
}

bool vector_insert(Vector* vec, size_t index, void* element) {
    if (!vec || index > vec->size) return false;
    
    if (!vector_ensure_capacity(vec, vec->size + 1)) {
        return false;
    }
    
    // Shift elements right
    for (size_t i = vec->size; i > index; i--) {
        vec->data[i] = vec->data[i - 1];
    }
    
    vec->data[index] = element;
    vec->size++;
    return true;
}

bool vector_remove(Vector* vec, size_t index) {
    if (!vec || index >= vec->size) return false;
    
    if (vec->element_free) {
        vec->element_free(vec->data[index]);
    }
    
    // Shift elements left
    for (size_t i = index; i < vec->size - 1; i++) {
        vec->data[i] = vec->data[i + 1];
    }
    
    vec->size--;
    return true;
}

void vector_clear(Vector* vec) {
    if (!vec) return;
    
    if (vec->element_free) {
        for (size_t i = 0; i < vec->size; i++) {
            vec->element_free(vec->data[i]);
        }
    }
    
    vec->size = 0;
}

size_t vector_size(Vector* vec) {
    return vec ? vec->size : 0;
}

bool vector_is_empty(Vector* vec) {
    return vec ? (vec->size == 0) : true;
}

size_t vector_capacity(Vector* vec) {
    return vec ? vec->capacity : 0;
}

bool vector_reserve(Vector* vec, size_t new_capacity) {
    if (!vec) return false;
    return vector_ensure_capacity(vec, new_capacity);
}

void vector_shrink_to_fit(Vector* vec) {
    if (!vec || vec->size == 0) return;
    
    if (vec->size < vec->capacity) {
        void** new_data = (void**)aether_caps_realloc(vec->data,
                                                      vec->capacity * sizeof(void*),
                                                      vec->size * sizeof(void*));
        if (new_data) {
            vec->data = new_data;
            vec->capacity = vec->size;
        }
    }
}

int vector_find(Vector* vec, const void* element, bool (*equals)(const void*, const void*)) {
    if (!vec || !equals) return -1;
    
    for (size_t i = 0; i < vec->size; i++) {
        if (equals(vec->data[i], element)) {
            return (int)i;
        }
    }
    
    return -1;
}

bool vector_contains(Vector* vec, const void* element, bool (*equals)(const void*, const void*)) {
    return vector_find(vec, element, equals) >= 0;
}

void vector_reverse(Vector* vec) {
    if (!vec || vec->size <= 1) return;
    
    for (size_t i = 0; i < vec->size / 2; i++) {
        void* temp = vec->data[i];
        vec->data[i] = vec->data[vec->size - 1 - i];
        vec->data[vec->size - 1 - i] = temp;
    }
}

void vector_sort(Vector* vec, int (*compare)(const void*, const void*)) {
    if (!vec || vec->size <= 1 || !compare) return;
    qsort(vec->data, vec->size, sizeof(void*), compare);
}

