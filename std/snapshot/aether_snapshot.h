/* MIT License (https://opensource.org/licenses/MIT)
 *
 * Portions copyright (c) 2026 Aether Developers.
 */
#ifndef AETHER_SNAPSHOT_H
#define AETHER_SNAPSHOT_H

/* std.snapshot — copy-on-write snapshot cell for read-mostly shared data
 * (issue #840).
 *
 * A snapshot cell holds a single atomic pointer to an immutable value
 * ("the snapshot"). Many actors read the current snapshot concurrently;
 * a single writer occasionally publishes a brand-new snapshot. Reads are
 * lock-free — a bare acquire load of the pointer — so readers never
 * block, never spin, never coordinate. This is the read-mostly answer
 * for config, routing tables, feature flags, and similar data that is
 * read on every request but rebuilt rarely.
 *
 *   cell = snapshot.new(initial)     // wrap an immutable value pointer
 *   cur  = snapshot.load(cell)       // lock-free read (acquire)
 *   old  = snapshot.store(cell, new) // publish `new`, return displaced
 *   ok   = snapshot.cas(cell, exp, new)  // RMW compare-and-swap
 *   snapshot.free(cell)              // free the cell (NOT the snapshots)
 *
 * RECLAMATION CONTRACT — read this, it is the whole point.
 *
 * Aether has no garbage collector. The cell stores ONLY the atomic
 * pointer; it does NOT own and never frees the snapshot values — those
 * are caller-owned. `store` and `cas` succeed by swapping the pointer
 * and RETURNING the displaced (old) snapshot so the WRITER can reclaim
 * it. But the displaced snapshot may still be in use by an in-flight
 * reader that loaded it microseconds before the swap. Freeing it
 * immediately is a use-after-free.
 *
 * The writer must therefore defer the free until a grace period has
 * elapsed during which every reader that could hold the old pointer has
 * finished its read. This is the standard RCU synchronize / call_rcu
 * discipline — the correct approach for a non-GC language, not a
 * workaround. In Aether's actor model the natural realization is a
 * single owning writer-actor that defers one generation: when it
 * publishes snapshot N+1 it frees snapshot N-1 (whose readers have, by
 * construction, all completed by then). See docs/snapshot-cell.md.
 *
 * Auto-freeing the displaced snapshot inside `store` would be a
 * use-after-free; this module deliberately does not do it.
 *
 * DO NOT use this for write-heavy data or for large values that change
 * often: every publish rebuilds the whole value, so writes are O(value
 * size). It is the read-mostly tool. (RWMutex is the trap this replaces:
 * a read lock still has a contended cache line and a write lock starves
 * readers; the COW cell's reads touch nothing shared but the pointer.)
 */

#include "../../runtime/aether_resource_caps.h" /* aether_caps_malloc / aether_caps_free */

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate a snapshot cell initialised to point at `initial` (which may
 * be NULL). The cell is cap-accounted via aether_caps_malloc. Returns
 * the cell handle (opaque `void*`), or NULL on allocation failure /
 * cap-exceeded. The cell does NOT take ownership of `initial`. */
void* aether_snapshot_new(void* initial);

/* Lock-free read: an atomic acquire load of the current snapshot
 * pointer. Readers never block. Passing a NULL cell returns NULL. The
 * returned pointer is valid for the caller's read; see the reclamation
 * contract for how long the writer must keep it alive. */
void* aether_snapshot_load(void* cell);

/* Atomically publish `new_value` (release store via exchange) and return
 * the DISPLACED (old) snapshot pointer. The caller (writer) now owns the
 * returned pointer and is responsible for reclaiming it after a grace
 * period — see the contract above. Passing a NULL cell returns NULL and
 * is otherwise a no-op. */
void* aether_snapshot_store(void* cell, void* new_value);

/* Compare-and-swap for read-modify-write loops. If the cell currently
 * holds `expected`, atomically replaces it with `new_value` and returns
 * 1; otherwise leaves the cell unchanged and returns 0. On success the
 * caller owns `expected` (the displaced snapshot) and must reclaim it
 * after a grace period. On failure the caller still owns `new_value`
 * (nothing was published) and typically re-loads + retries. Passing a
 * NULL cell returns 0. */
int aether_snapshot_cas(void* cell, void* expected, void* new_value);

/* Free the cell itself. Does NOT free whatever snapshot the cell points
 * at — that value is caller-owned and may still be live elsewhere. NULL
 * is a no-op (mirrors libc free). */
void aether_snapshot_free(void* cell);

#ifdef __cplusplus
}
#endif

#endif /* AETHER_SNAPSHOT_H */
