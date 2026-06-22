#ifndef AETHER_CAPSICUM_H
#define AETHER_CAPSICUM_H

// std.capsicum — thin bindings over FreeBSD's Capsicum capability API.
//
// Capsicum is FreeBSD's OS-enforced sandbox: a process calls cap_enter()
// to drop into "capability mode", after which it can no longer reach any
// global namespace (open paths, bind sockets, look up PIDs) — it may act
// only on file descriptors it already holds, and only within the rights
// each fd was limited to. Unlike Aether's LD_PRELOAD layer this cannot be
// bypassed from userspace: the kernel enforces it.
//
// These wrappers are intentionally minimal — they expose cap_enter,
// cap_getmode, cap_rights_limit and cap_fcntls_limit so hand-written
// Aether code can sandbox itself. Wiring Capsicum into spawn_sandboxed
// transparently is a later phase (see docs/aether_compared_to_capsicum.md).
//
// Off FreeBSD every entry point is a stub returning AETHER_CAP_UNSUPPORTED
// so portable Aether code can probe capsicum_available() and degrade.

// Return codes shared by the wrappers below.
#define AETHER_CAP_OK            0   // operation succeeded
#define AETHER_CAP_ERR          -1   // syscall failed (see errno on FreeBSD)
#define AETHER_CAP_UNSUPPORTED  -2   // not built on FreeBSD / kernel lacks it

// cap_rights_t is an opaque struct on modern FreeBSD and cannot be
// constructed from Aether. Instead Aether passes a plain integer bitmask
// of the AETHER_CAPR_* bits below; aether_cap_rights_limit() translates
// each set bit into a cap_rights_set() call. These values are this
// module's own ABI — they are NOT the kernel's CAP_* constants.
#define AETHER_CAPR_READ     0x0001  // CAP_READ
#define AETHER_CAPR_WRITE    0x0002  // CAP_WRITE
#define AETHER_CAPR_SEEK     0x0004  // CAP_SEEK
#define AETHER_CAPR_FSTAT    0x0008  // CAP_FSTAT
#define AETHER_CAPR_FTRUNC   0x0010  // CAP_FTRUNCATE
#define AETHER_CAPR_FSYNC    0x0020  // CAP_FSYNC
#define AETHER_CAPR_MMAP     0x0040  // CAP_MMAP
#define AETHER_CAPR_ACCEPT   0x0080  // CAP_ACCEPT
#define AETHER_CAPR_CONNECT  0x0100  // CAP_CONNECT
#define AETHER_CAPR_BIND     0x0200  // CAP_BIND
#define AETHER_CAPR_LISTEN   0x0400  // CAP_LISTEN
#define AETHER_CAPR_RECV     0x0800  // CAP_RECV (alias of CAP_READ for sockets)
#define AETHER_CAPR_SEND     0x1000  // CAP_SEND (alias of CAP_WRITE for sockets)
#define AETHER_CAPR_EVENT    0x2000  // CAP_EVENT (kqueue/select on the fd)
#define AETHER_CAPR_IOCTL    0x4000  // CAP_IOCTL
#define AETHER_CAPR_FCNTL    0x8000  // CAP_FCNTL

// Is Capsicum usable in this process right now? Returns 1 on FreeBSD
// whose kernel reports the security_capabilities feature, 0 otherwise
// (non-FreeBSD build, or a kernel built without Capsicum support).
int aether_capsicum_available(void);

// Enter capability mode. IRREVERSIBLE and inherited by children. After
// this returns AETHER_CAP_OK the process can no longer open new paths,
// bind sockets, or reach any global namespace. Returns AETHER_CAP_ERR if
// the cap_enter syscall fails, AETHER_CAP_UNSUPPORTED off FreeBSD.
int aether_capsicum_enter(void);

// Report whether the process is already in capability mode.
// Returns 1 if sandboxed, 0 if not, AETHER_CAP_UNSUPPORTED off FreeBSD.
int aether_capsicum_in_mode(void);

// Limit an open fd to the rights named by `rights_mask` (a bitwise-OR
// of AETHER_CAPR_* values). Rights can only ever be narrowed. Returns
// AETHER_CAP_OK / AETHER_CAP_ERR / AETHER_CAP_UNSUPPORTED.
int aether_capsicum_rights_limit(int fd, int rights_mask);

// Limit which fcntl(2) commands are permitted on an fd. `fcntl_mask` is
// a bitwise-OR of AETHER_CAPF_* values below. Returns the usual codes.
#define AETHER_CAPF_GETFL  0x01  // CAP_FCNTL_GETFL
#define AETHER_CAPF_SETFL  0x02  // CAP_FCNTL_SETFL
#define AETHER_CAPF_GETOWN 0x04  // CAP_FCNTL_GETOWN
#define AETHER_CAPF_SETOWN 0x08  // CAP_FCNTL_SETOWN
int aether_capsicum_fcntls_limit(int fd, int fcntl_mask);

#endif // AETHER_CAPSICUM_H
