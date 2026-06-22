// aether_capsicum.c — FreeBSD Capsicum bindings for std.capsicum.
//
// On FreeBSD these wrap cap_enter / cap_getmode / cap_rights_limit /
// cap_fcntls_limit from <sys/capsicum.h>. On every other platform they
// are stubs returning AETHER_CAP_UNSUPPORTED, so Aether code that
// imports std.capsicum still compiles and links everywhere — it just
// needs to check capsicum_available() before relying on enforcement.
//
// See aether_capsicum.h for the API contract and the AETHER_CAPR_*
// bitmask ABI. cap_rights_t is opaque on modern FreeBSD, so the rights
// translation (integer mask -> cap_rights_set calls) lives here and is
// never exposed to Aether.

#include "aether_capsicum.h"

#if defined(__FreeBSD__)

#include <sys/capsicum.h>
#include <unistd.h>      // feature_present
#include <errno.h>

int aether_capsicum_available(void) {
    // feature_present() returns 1 only when the kernel was built with
    // Capsicum support (options CAPABILITY_MODE / CAPABILITIES, both in
    // GENERIC). 0 on absence or error — which is the safe answer.
    return feature_present("security_capabilities") ? 1 : 0;
}

int aether_capsicum_enter(void) {
    return (cap_enter() == 0) ? AETHER_CAP_OK : AETHER_CAP_ERR;
}

int aether_capsicum_in_mode(void) {
    u_int mode = 0;
    if (cap_getmode(&mode) != 0) {
        // ENOSYS here means the kernel lacks Capsicum — report "not in
        // capability mode" rather than an error; the process plainly
        // isn't sandboxed.
        if (errno == ENOSYS) return 0;
        return AETHER_CAP_ERR;
    }
    return mode ? 1 : 0;
}

// Translate the AETHER_CAPR_* integer mask into a populated cap_rights_t.
// cap_rights_init() must be called first (it zeroes the version field);
// cap_rights_set() then adds each requested right.
static void build_rights(cap_rights_t* r, int mask) {
    cap_rights_init(r);  // empty rights set, correctly versioned
    if (mask & AETHER_CAPR_READ)    cap_rights_set(r, CAP_READ);
    if (mask & AETHER_CAPR_WRITE)   cap_rights_set(r, CAP_WRITE);
    if (mask & AETHER_CAPR_SEEK)    cap_rights_set(r, CAP_SEEK);
    if (mask & AETHER_CAPR_FSTAT)   cap_rights_set(r, CAP_FSTAT);
    if (mask & AETHER_CAPR_FTRUNC)  cap_rights_set(r, CAP_FTRUNCATE);
    if (mask & AETHER_CAPR_FSYNC)   cap_rights_set(r, CAP_FSYNC);
    if (mask & AETHER_CAPR_MMAP)    cap_rights_set(r, CAP_MMAP);
    if (mask & AETHER_CAPR_ACCEPT)  cap_rights_set(r, CAP_ACCEPT);
    if (mask & AETHER_CAPR_CONNECT) cap_rights_set(r, CAP_CONNECT);
    if (mask & AETHER_CAPR_BIND)    cap_rights_set(r, CAP_BIND);
    if (mask & AETHER_CAPR_LISTEN)  cap_rights_set(r, CAP_LISTEN);
    if (mask & AETHER_CAPR_RECV)    cap_rights_set(r, CAP_RECV);
    if (mask & AETHER_CAPR_SEND)    cap_rights_set(r, CAP_SEND);
    if (mask & AETHER_CAPR_EVENT)   cap_rights_set(r, CAP_EVENT);
    if (mask & AETHER_CAPR_IOCTL)   cap_rights_set(r, CAP_IOCTL);
    if (mask & AETHER_CAPR_FCNTL)   cap_rights_set(r, CAP_FCNTL);
}

int aether_capsicum_rights_limit(int fd, int rights_mask) {
    if (fd < 0) return AETHER_CAP_ERR;
    cap_rights_t rights;
    build_rights(&rights, rights_mask);
    return (cap_rights_limit(fd, &rights) == 0) ? AETHER_CAP_OK
                                                : AETHER_CAP_ERR;
}

int aether_capsicum_fcntls_limit(int fd, int fcntl_mask) {
    if (fd < 0) return AETHER_CAP_ERR;
    uint32_t fc = 0;
    if (fcntl_mask & AETHER_CAPF_GETFL)  fc |= CAP_FCNTL_GETFL;
    if (fcntl_mask & AETHER_CAPF_SETFL)  fc |= CAP_FCNTL_SETFL;
    if (fcntl_mask & AETHER_CAPF_GETOWN) fc |= CAP_FCNTL_GETOWN;
    if (fcntl_mask & AETHER_CAPF_SETOWN) fc |= CAP_FCNTL_SETOWN;
    return (cap_fcntls_limit(fd, fc) == 0) ? AETHER_CAP_OK
                                           : AETHER_CAP_ERR;
}

#else // not FreeBSD — Capsicum does not exist

int aether_capsicum_available(void) { return 0; }

int aether_capsicum_enter(void) { return AETHER_CAP_UNSUPPORTED; }

int aether_capsicum_in_mode(void) { return AETHER_CAP_UNSUPPORTED; }

int aether_capsicum_rights_limit(int fd, int rights_mask) {
    (void)fd; (void)rights_mask;
    return AETHER_CAP_UNSUPPORTED;
}

int aether_capsicum_fcntls_limit(int fd, int fcntl_mask) {
    (void)fd; (void)fcntl_mask;
    return AETHER_CAP_UNSUPPORTED;
}

#endif // __FreeBSD__
