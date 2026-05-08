/* aether_capsicum.c — Capsicum capability-based security (FreeBSD).
 *
 * Capsicum is a sandbox framework available on FreeBSD 10.0+ that restricts
 * file descriptor capabilities at the OS level. This module provides Aether
 * bindings to the core Capsicum syscalls:
 * - cap_enter(2): enter irreversible capability mode
 * - cap_rights_limit(2): restrict an fd to specific capabilities
 * - cap_pdwait4(2): wait on a sandboxed child process descriptor
 * - cap_getmode(2): check if in capability mode (for availability check)
 *
 * The bindings are minimal wrappers that return errno-style results
 * (0 on success, -1 on error). */

#include "../../runtime/config/aether_optimization_config.h"

#include <sys/types.h>

#ifdef __FreeBSD__

#include <sys/capsicum.h>
#include <string.h>

int capsicum_available(void) {
  cap_mode_t mode;
  int result = cap_getmode(&mode);
  if (result == -1) {
    return 0;  /* Not available or not in capability mode. */
  }
  return 1;  /* Capsicum is available. */
}

int capsicum_enter(void) {
  if (cap_enter() == -1) {
    return -1;
  }
  return 0;
}

int capsicum_limit_rights(int fd, uint64_t rights) {
  cap_rights_t rights_struct;
  cap_rights_init(&rights_struct, rights);
  if (cap_rights_limit(fd, &rights_struct) == -1) {
    return -1;
  }
  return 0;
}

int capsicum_pdwait4(int pd, int* status_ptr, int options, void* rusage_ptr) {
  pid_t result;
  if (rusage_ptr) {
    result = cap_pdwait4(pd, status_ptr, options, (struct rusage*)rusage_ptr);
  } else {
    result = cap_pdwait4(pd, status_ptr, options, NULL);
  }
  if (result == -1) {
    return -1;
  }
  return (int)result;
}

#else

/* Stub implementations for non-FreeBSD platforms or when AETHER_HAS_CAPSICUM
 * is not defined. These always fail with -1, signaling that Capsicum is not
 * available. Aether code should guard calls with capsicum_available(). */

int capsicum_available(void) { return 0; }
int capsicum_enter(void) { return -1; }
int capsicum_limit_rights(int fd, uint64_t rights) { (void)fd; (void)rights; return -1; }
int capsicum_pdwait4(int pd, int* status_ptr, int options, void* rusage_ptr) {
  (void)pd; (void)status_ptr; (void)options; (void)rusage_ptr;
  return -1;
}

#endif
