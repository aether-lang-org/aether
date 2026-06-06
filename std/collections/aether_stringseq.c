#include "aether_stringseq.h"
#include "../string/aether_string.h"
#include "../../runtime/aether_resource_caps.h"

#include <stdlib.h>
#include <string.h>

/* AetherStringArray surface used by `string_seq_to_array`. The full
 * definition lives in std/string/aether_string.h, but only that
 * file exposes the type — for our purposes we only need to allocate
 * one and populate it via string_array_size / get / new helpers, so
 * forward-declare the surface and call the constructors via the
 * exposed functions. To avoid a layering tangle the conversion path
 * here builds the array by walking and `malloc`ing in-place. */
typedef struct {
    AetherString** strings;
    size_t count;
} AetherStringArrayLayout;

StringSeq* string_seq_empty(void) {
    return NULL;
}

StringSeq* string_seq_cons(const char* head, StringSeq* tail) {
    /* #463: cap-aware cons cell. Fixed sizeof(StringSeq); the
     * matching free in string_seq_free passes the same size. */
    StringSeq* cell = (StringSeq*)aether_caps_malloc(sizeof(StringSeq));
    if (!cell) return NULL;
    cell->ref_count = 1;
    cell->length = (tail ? tail->length : 0) + 1;
    cell->head = (void*)head;
    /* Retain the head so the cell holds an independent reference. The
     * `string_retain` helper is NULL-safe and a no-op on plain
     * `char*` literals (the magic-byte check fails), so const
     * literals pass through unchanged. */
    string_retain(head);
    /* Retain tail so the cell holds its own reference; callers
     * transferring ownership drop their local with `string_seq_free`
     * immediately after the cons. */
    cell->tail = string_seq_retain(tail);
    return cell;
}

const char* string_seq_head(StringSeq* s) {
    /* Cast through `const char*` is safe: the head is one of an
     * AetherString* (recognised via the magic-byte check) or a
     * plain `const char*` literal. Either way the caller will
     * dispatch on the magic when it consumes the value. */
    return s ? (const char*)s->head : "";
}

StringSeq* string_seq_tail(StringSeq* s) {
    return s ? s->tail : NULL;
}

int string_seq_is_empty(StringSeq* s) {
    return s == NULL ? 1 : 0;
}

int string_seq_length(StringSeq* s) {
    return s ? s->length : 0;
}

StringSeq* string_seq_retain(StringSeq* s) {
    if (s) s->ref_count++;
    return s;
}

void string_seq_free(StringSeq* s) {
    /* Iterative spine walk — deep lists must not blow the stack.
     * Stop at the first cell whose refcount remains >0 after our
     * decrement: the other owner will eventually run its own free
     * and continue the walk past that point. */
    while (s) {
        if (--s->ref_count > 0) {
            return;
        }
        StringSeq* next = s->tail;
        string_release(s->head);
        aether_caps_free(s, sizeof(StringSeq));
        s = next;
    }
}

StringSeq* string_seq_from_array(void* arr_v, int count) {
    if (!arr_v || count <= 0) return NULL;
    /* `arr_v` is an `AetherStringArray*` (the shape `string.split`
     * returns) — first field is `AetherString** strings`, second is
     * `size_t count`. We use the caller-supplied `count` rather than
     * the struct's so that callers passing a longer array can sub-
     * sequence it via this entry point. The cast is via the struct
     * shape, not via `const char**`, so that `strings[i]` resolves
     * to the AetherString* at index i (each carrying a magic header
     * `string_retain` recognises) rather than to a raw word from the
     * struct header. */
    AetherStringArrayLayout* arr = (AetherStringArrayLayout*)arr_v;
    StringSeq* head = NULL;
    /* Build back-to-front so we cons each element in O(1). */
    for (int i = count - 1; i >= 0; i--) {
        AetherString* elem = arr->strings[i];
        StringSeq* cell = string_seq_cons((const char*)elem, head);
        if (!cell) {
            string_seq_free(head);
            return NULL;
        }
        /* cons retained the prior `head`; drop our local ref so the
         * new cell holds the only one. Safe even when head is NULL
         * (free is a no-op). */
        string_seq_free(head);
        head = cell;
    }
    return head;
}

/* Closure-free combinators. See the contracts in aether_stringseq.h.
 * All four maintain the refcount invariant: every cell in any
 * returned spine is reachable through exactly the refs the caller
 * holds, and freeing the result drops exactly the right number of
 * refs from any cells shared with the input. */

StringSeq* string_seq_reverse(StringSeq* s) {
    /* Walk the source spine, prepending each head to a fresh result
     * spine. Each `cons` retains the head (the underlying string
     * gets a new ref); we don't share any tail cells with the
     * source, so freeing the result later is independent of `s`. */
    StringSeq* result = NULL;
    StringSeq* cur = s;
    while (cur) {
        StringSeq* cell = string_seq_cons((const char*)cur->head, result);
        if (!cell) {
            string_seq_free(result);
            return NULL;
        }
        /* cons retained `result` (the prior head of the new spine);
         * we hold that ref ourselves through `result`. Drop our
         * local now so the new cell is the sole owner of the rest
         * of the chain. Safe even when result is NULL. */
        string_seq_free(result);
        result = cell;
        cur = cur->tail;
    }
    return result;
}

StringSeq* string_seq_concat(StringSeq* a, StringSeq* b) {
    /* Strategy: reverse `a`, then iteratively cons each element of
     * the reversed list onto `b` (with `b` shared). Net effect: a
     * fresh spine of length |a| whose tail is `b` itself,
     * refcount-bumped via cons. Two passes over `a` (one to
     * reverse, one to walk the reversed view), but each is O(|a|)
     * and `b` is never walked. */
    if (!a) return string_seq_retain(b);

    StringSeq* reversed = string_seq_reverse(a);
    if (!reversed) return NULL;

    StringSeq* result = b;
    string_seq_retain(result); /* match the cons retain pattern below */
    StringSeq* cur = reversed;
    while (cur) {
        StringSeq* cell = string_seq_cons((const char*)cur->head, result);
        if (!cell) {
            string_seq_free(result);
            string_seq_free(reversed);
            return NULL;
        }
        string_seq_free(result);
        result = cell;
        cur = cur->tail;
    }
    string_seq_free(reversed);
    return result;
}

StringSeq* string_seq_take(StringSeq* s, int n) {
    /* Build the first n elements as a fresh independent spine.
     * Walk the source forward collecting elements into a temp
     * reverse buffer (head-of-result-on-the-end so every step is
     * O(1)), then reverse at the end. */
    if (n <= 0 || !s) return NULL;

    StringSeq* reversed_prefix = NULL;
    StringSeq* cur = s;
    int taken = 0;
    while (cur && taken < n) {
        StringSeq* cell = string_seq_cons((const char*)cur->head, reversed_prefix);
        if (!cell) {
            string_seq_free(reversed_prefix);
            return NULL;
        }
        string_seq_free(reversed_prefix);
        reversed_prefix = cell;
        cur = cur->tail;
        taken++;
    }
    StringSeq* result = string_seq_reverse(reversed_prefix);
    string_seq_free(reversed_prefix);
    return result;
}

StringSeq* string_seq_drop(StringSeq* s, int n) {
    /* Walk forward n cells (or until the spine ends) and retain the
     * cell we land on. No allocations — the result shares storage
     * with the source, so the caller owning a ref to the returned
     * cell will keep that sub-spine alive even if the caller's
     * handle to `s` is freed first. */
    if (n <= 0) return string_seq_retain(s);
    StringSeq* cur = s;
    int skipped = 0;
    while (cur && skipped < n) {
        cur = cur->tail;
        skipped++;
    }
    return string_seq_retain(cur);
}

void* string_seq_to_array(StringSeq* s) {
    if (!s) return NULL;
    int n = s->length;
    /* #463 cross-file T1 pair: this AetherStringArrayLayout is
     * layout-compatible with std/string's AetherStringArray
     * {AetherString** strings; size_t count;} and is freed by
     * string_array_free over there — which now uses aether_caps_free
     * with `count * sizeof(AetherString*)` for the strings array and
     * `sizeof(AetherStringArray)` for the struct. Allocate with the
     * matching caps shapes so the cross-file free accounts correctly
     * (this is why the two sides convert in the same commit). */
    AetherStringArrayLayout* arr = (AetherStringArrayLayout*)aether_caps_malloc(
        sizeof(AetherStringArrayLayout));
    if (!arr) return NULL;
    arr->strings = (AetherString**)aether_caps_malloc(sizeof(AetherString*) * (size_t)n);
    if (!arr->strings) {
        aether_caps_free(arr, sizeof(AetherStringArrayLayout));
        return NULL;
    }
    arr->count = (size_t)n;
    /* Walk the spine, retaining each head into the destination array.
     * AetherStringArray's contract (per std/string/aether_string.c) is
     * that the array owns the AetherString*s and releases them on
     * `string_array_free`. So we retain to balance that. */
    StringSeq* cur = s;
    int i = 0;
    while (cur && i < n) {
        AetherString* as = (AetherString*)cur->head;
        string_retain(as);
        arr->strings[i++] = as;
        cur = cur->tail;
    }
    return arr;
}
