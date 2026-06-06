#include "aether_stringlist.h"
#include "aether_collections.h"
#include "../string/aether_string.h"
#include "../../runtime/aether_resource_caps.h"

#include <stdlib.h>

/* StringList is a thin layer over the existing ArrayList. We could
 * have reused ArrayList directly with retain/release sprinkled on
 * top, but a distinct type makes the contract clear in C consumers
 * and lets `string_list_*` be safe to call even when the user mixed
 * a plain ArrayList in by accident — they'd get a NULL back rather
 * than corrupting refcounts. */
struct StringList {
    ArrayList* items;
};

StringList* string_list_new(void) {
    /* #463: cap-aware wrapper struct. The backing ArrayList is
     * already caps-aware (#471); only the StringList shell
     * converts here. */
    StringList* sl = (StringList*)aether_caps_malloc(sizeof(StringList));
    if (!sl) return NULL;
    sl->items = list_new();
    if (!sl->items) {
        aether_caps_free(sl, sizeof(StringList));
        return NULL;
    }
    return sl;
}

int string_list_add(StringList* list, const void* s) {
    if (!list || !list->items) return 0;
    /* Take a strong reference before storing. string_retain is
     * NULL-safe and a no-op on plain `char*` (the magic check
     * fails), so literals pass through unchanged. */
    string_retain(s);
    if (!list_add_raw(list->items, (void*)s)) {
        /* The list grew but realloc failed — release the ref we
         * just took so the caller's accounting stays balanced. */
        string_release(s);
        return 0;
    }
    return 1;
}

const void* string_list_get(StringList* list, int index) {
    if (!list || !list->items) return NULL;
    return list_get_raw(list->items, index);
}

void string_list_set(StringList* list, int index, const void* s) {
    if (!list || !list->items) return;
    if (index < 0 || index >= list_size(list->items)) return;
    /* Release the previous occupant, then retain and store the new
     * one. Releasing first means a self-set
     * (`string_list_set(L, i, string_list_get(L, i))`) doesn't
     * accidentally drop the only reference between the release and
     * retain — string_retain bumps the refcount before string_release
     * decrements it. Wait, no: that ordering is wrong. We need to
     * retain first, *then* release the old. */
    const void* old = list_get_raw(list->items, index);
    string_retain(s);
    list_set(list->items, index, (void*)s);
    string_release(old);
}

int string_list_size(StringList* list) {
    if (!list || !list->items) return -1;
    return list_size(list->items);
}

void string_list_remove(StringList* list, int index) {
    if (!list || !list->items) return;
    if (index < 0 || index >= list_size(list->items)) return;
    const void* old = list_get_raw(list->items, index);
    list_remove(list->items, index);
    string_release(old);
}

void string_list_clear(StringList* list) {
    if (!list || !list->items) return;
    int n = list_size(list->items);
    for (int i = 0; i < n; i++) {
        const void* item = list_get_raw(list->items, i);
        string_release(item);
    }
    list_clear(list->items);
}

void string_list_free(StringList* list) {
    if (!list) return;
    if (list->items) {
        int n = list_size(list->items);
        for (int i = 0; i < n; i++) {
            const void* item = list_get_raw(list->items, i);
            string_release(item);
        }
        list_free(list->items);
    }
    aether_caps_free(list, sizeof(StringList));
}
