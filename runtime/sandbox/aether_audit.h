#ifndef AETHER_AUDIT_H
#define AETHER_AUDIT_H

// aether_audit.h — sandbox audit trail for the in-process permission layer.
//
// The LD_PRELOAD layer (libaether_sandbox_preload.c) already logs denied
// libc operations. The in-process checker — the one the stdlib grant
// checks and embedded/hosted plugins go through — previously logged
// nothing. This module gives that layer an audit trail with two faces:
//
//   1. A live sink, controlled by the AETHER_SANDBOX_AUDIT environment
//      variable (none | stderr | file), mirroring AETHER_SANDBOX_LOG.
//      Default is "none": unlike denial-only logging, auditing records
//      *allowed* checks too and is therefore opt-in.
//
//   2. An in-memory ring buffer the program can query at runtime via
//      std.audit (audit.count / audit.entry / audit.clear).
//
// aether_sandbox_audit() is called by the compiler-emitted in-process
// checker on every permission check. It is a cheap no-op when the sink
// is "none" and the ring buffer has not been asked for — see the impl.

// Record one permission check. `allowed` is 1 if the check passed, 0 if
// it was denied. Safe to call from any thread.
void aether_sandbox_audit(const char* category, const char* resource,
                          int allowed);

// ---- Query API (backs std.audit) ----

// Number of entries currently held in the ring buffer.
int aether_audit_count(void);

// Field accessors for entry `i` (0-based, oldest first). Out-of-range
// `i` yields "" / -1. The returned string points into the ring buffer
// and stays valid only until that slot is overwritten by a later
// aether_sandbox_audit() call or dropped by aether_audit_clear(). A
// caller that records audit entries concurrently with querying should
// copy the string promptly; the std.audit wrappers do.
const char* aether_audit_entry_category(int i);
const char* aether_audit_entry_resource(int i);
int         aether_audit_entry_allowed(int i);   // 1 allowed, 0 denied

// Drop all buffered entries. Does not affect the live sink.
void aether_audit_clear(void);

#endif // AETHER_AUDIT_H
