/* MIT License (https://opensource.org/licenses/MIT)
 *
 * Portions copyright (c) 2026 Aether Developers.
 */

/* aether_snapshot.c — copy-on-write snapshot cell (issue #840).
 *
 * Implementation notes:
 *
 *   * The cell is a single C11 `_Atomic(void*)`. That is the entire
 *     state — no lock, no version counter, no reader list. Reads are a
 *     pure acquire load; publishes are a release exchange / CAS.
 *
 *   * Memory ordering. Acquire on load pairs with release on store/CAS
 *     so that a reader which observes a freshly-published pointer also
 *     observes every write the writer made while building that snapshot
 *     (the value the pointer points at is fully constructed before it is
 *     published). This is the publish/subscribe pattern: build value,
 *     release-store the pointer; acquire-load the pointer, then read the
 *     value. Relaxed ordering would let a reader see the new pointer but
 *     a stale value — a real bug — so acquire/release is mandatory, not
 *     a tuning knob.
 *
 *   * Reclamation is NOT done here. store/cas return the displaced
 *     pointer; the writer owns reclaiming it after a grace period. See
 *     aether_snapshot.h and docs/snapshot-cell.md for the contract and
 *     why auto-freeing here would be a use-after-free.
 *
 *   * Cap accounting. The cell struct is allocated through
 *     aether_caps_malloc / freed through aether_caps_free so it is
 *     consistent with the process-wide memory cap (the caps audit). The
 *     snapshot VALUES are not accounted here — they are caller-owned and
 *     accounted (or not) by whoever allocates them.
 */

#include "aether_snapshot.h"

#include <stdatomic.h>
#include <stddef.h>

/* The cell. A single atomic pointer; nothing else is shared state. */
typedef struct aether_snapshot_cell {
    _Atomic(void*) ptr;
} aether_snapshot_cell;

void* aether_snapshot_new(void* initial) {
    aether_snapshot_cell* cell =
        (aether_snapshot_cell*)aether_caps_malloc(sizeof *cell);
    if (!cell) {
        return NULL; /* cap-exceeded or OOM */
    }
    /* No reader can see `cell` yet (we have not returned it), so a
     * relaxed init store is sufficient and correct — the acquire/release
     * pairing only matters once the cell is shared. */
    atomic_store_explicit(&cell->ptr, initial, memory_order_relaxed);
    return cell;
}

void* aether_snapshot_load(void* cell_v) {
    aether_snapshot_cell* cell = (aether_snapshot_cell*)cell_v;
    if (!cell) {
        return NULL;
    }
    /* Lock-free acquire load. Pairs with the release store/exchange in
     * publish so the snapshot value seen through this pointer is fully
     * constructed. */
    return atomic_load_explicit(&cell->ptr, memory_order_acquire);
}

void* aether_snapshot_store(void* cell_v, void* new_value) {
    aether_snapshot_cell* cell = (aether_snapshot_cell*)cell_v;
    if (!cell) {
        return NULL;
    }
    /* Release exchange: publish `new_value` and hand the writer back the
     * pointer it just displaced. acq_rel so the publish has release
     * semantics (the value is visible) and the returned old pointer load
     * has acquire semantics (consistent with concurrent CAS attempts). */
    return atomic_exchange_explicit(&cell->ptr, new_value,
                                    memory_order_acq_rel);
}

int aether_snapshot_cas(void* cell_v, void* expected, void* new_value) {
    aether_snapshot_cell* cell = (aether_snapshot_cell*)cell_v;
    if (!cell) {
        return 0;
    }
    /* Strong CAS — no spurious failures, which keeps the Aether-side
     * retry loop simple (a failure means a real concurrent publish, not
     * noise). Success path is release (publish the new value); failure
     * path is acquire (so the reloaded `expected` is a consistent view
     * for the next retry). `expected` is passed by value, not by
     * pointer: on failure the Aether caller re-runs load() to get the
     * fresh current value rather than relying on a written-back expected,
     * which keeps the extern signature a plain `(ptr,ptr,ptr)->int`. */
    void* exp = expected;
    return atomic_compare_exchange_strong_explicit(
               &cell->ptr, &exp, new_value,
               memory_order_release, memory_order_acquire)
               ? 1
               : 0;
}

void aether_snapshot_free(void* cell_v) {
    aether_snapshot_cell* cell = (aether_snapshot_cell*)cell_v;
    if (!cell) {
        return; /* NULL-safe, mirrors libc free */
    }
    /* Free ONLY the cell. The snapshot value the cell points at is
     * caller-owned and may still be live; freeing it here would be a
     * use-after-free for any in-flight reader and a double-free for a
     * writer following the reclamation contract. */
    aether_caps_free(cell, sizeof *cell);
}
