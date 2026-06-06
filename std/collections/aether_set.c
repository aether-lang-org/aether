#include "aether_set.h"
#include "../../runtime/aether_resource_caps.h"
#include <stdlib.h>

#define DUMMY_VALUE ((void*)1)

Set* set_create(size_t initial_capacity,
               uint64_t (*hash_func)(const void*),
               bool (*key_equals)(const void*, const void*),
               void (*key_free)(void*),
               void* (*key_clone)(const void*)) {
    
    /* #463: cap-aware wrapper struct. The backing hashmap is already
     * caps-aware (aether_hashmap.c, #469); only the Set shell
     * converts here. */
    Set* set = (Set*)aether_caps_malloc(sizeof(Set));
    if (!set) return NULL;

    set->map = hashmap_create(initial_capacity, hash_func, key_equals,
                             key_free, NULL, key_clone, NULL);

    if (!set->map) {
        aether_caps_free(set, sizeof(Set));
        return NULL;
    }

    return set;
}

void set_free(Set* set) {
    if (!set) return;
    hashmap_free(set->map);
    aether_caps_free(set, sizeof(Set));
}

bool set_add(Set* set, void* element) {
    if (!set) return false;
    return hashmap_insert(set->map, element, DUMMY_VALUE);
}

bool set_remove(Set* set, const void* element) {
    if (!set) return false;
    return hashmap_remove(set->map, element);
}

bool set_contains(Set* set, const void* element) {
    if (!set) return false;
    return hashmap_contains(set->map, element);
}

void set_clear(Set* set) {
    if (!set) return;
    hashmap_clear(set->map);
}

size_t set_size(Set* set) {
    return set ? hashmap_size(set->map) : 0;
}

bool set_is_empty(Set* set) {
    return set ? hashmap_is_empty(set->map) : true;
}

// Set union: all elements in A or B
Set* set_union(Set* a, Set* b) {
    if (!a || !b) return NULL;
    
    Set* result = set_create(hashmap_size(a->map) + hashmap_size(b->map),
                            a->map->hash_func,
                            a->map->key_equals,
                            a->map->key_free,
                            a->map->key_clone);
    
    if (!result) return NULL;
    
    // Add all elements from A
    SetIterator iter_a = set_iterator(a);
    void* element;
    while (set_iterator_next(&iter_a, &element)) {
        void* cloned = a->map->key_clone ? a->map->key_clone(element) : element;
        set_add(result, cloned);
    }
    
    // Add all elements from B
    SetIterator iter_b = set_iterator(b);
    while (set_iterator_next(&iter_b, &element)) {
        if (!set_contains(result, element)) {
            void* cloned = b->map->key_clone ? b->map->key_clone(element) : element;
            set_add(result, cloned);
        }
    }
    
    return result;
}

// Set intersection: elements in both A and B
Set* set_intersection(Set* a, Set* b) {
    if (!a || !b) return NULL;
    
    Set* result = set_create(hashmap_size(a->map) < hashmap_size(b->map) ?
                            hashmap_size(a->map) : hashmap_size(b->map),
                            a->map->hash_func,
                            a->map->key_equals,
                            a->map->key_free,
                            a->map->key_clone);
    
    if (!result) return NULL;
    
    // Iterate through smaller set for efficiency
    Set* smaller = hashmap_size(a->map) < hashmap_size(b->map) ? a : b;
    Set* larger = smaller == a ? b : a;
    
    SetIterator iter = set_iterator(smaller);
    void* element;
    while (set_iterator_next(&iter, &element)) {
        if (set_contains(larger, element)) {
            void* cloned = smaller->map->key_clone ? smaller->map->key_clone(element) : element;
            set_add(result, cloned);
        }
    }
    
    return result;
}

// Set difference: elements in A but not in B
Set* set_difference(Set* a, Set* b) {
    if (!a || !b) return NULL;
    
    Set* result = set_create(hashmap_size(a->map),
                            a->map->hash_func,
                            a->map->key_equals,
                            a->map->key_free,
                            a->map->key_clone);
    
    if (!result) return NULL;
    
    SetIterator iter = set_iterator(a);
    void* element;
    while (set_iterator_next(&iter, &element)) {
        if (!set_contains(b, element)) {
            void* cloned = a->map->key_clone ? a->map->key_clone(element) : element;
            set_add(result, cloned);
        }
    }
    
    return result;
}

bool set_is_subset(Set* a, Set* b) {
    if (!a || !b) return false;
    if (hashmap_size(a->map) > hashmap_size(b->map)) return false;
    
    SetIterator iter = set_iterator(a);
    void* element;
    while (set_iterator_next(&iter, &element)) {
        if (!set_contains(b, element)) {
            return false;
        }
    }
    
    return true;
}

bool set_is_superset(Set* a, Set* b) {
    return set_is_subset(b, a);
}

SetIterator set_iterator(Set* set) {
    return hashmap_iterator(set ? set->map : NULL);
}

bool set_iterator_next(SetIterator* iter, void** element) {
    return hashmap_iterator_next(iter, element, NULL);
}

Set* set_create_string(size_t initial_capacity) {
    return set_create(initial_capacity,
                     hashmap_hash_string,
                     hashmap_equals_string,
                     hashmap_free_string,
                     hashmap_clone_string);
}

Set* set_create_int(size_t initial_capacity) {
    return set_create(initial_capacity,
                     hashmap_hash_int,
                     hashmap_equals_int,
                     hashmap_free_int,
                     hashmap_clone_int);
}

