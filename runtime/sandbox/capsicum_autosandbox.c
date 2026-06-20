// capsicum_autosandbox.c — opt-in OS-enforced self-sandbox at startup.
//
// The compiler-emitted main() calls aether_capsicum_autosandbox() once,
// immediately after aether_args_init() and before any user code. If the
// environment variable AETHER_CAPSICUM is set to "1", the process enters
// FreeBSD capability mode (cap_enter) right there.
//
// Why a startup hook rather than parent-side enforcement: cap_enter() is
// irreversible and forbids reaching the global filesystem namespace —
// which includes the runtime linker. A parent therefore CANNOT cap_enter
// a child and then exec a dynamically linked binary: rtld would be unable
// to open ld-elf.so.1 / libc. The only model that works for dynamic
// binaries is self-sandboxing: the child execs normally, rtld loads every
// shared library, and only THEN — here, at the top of main() — the
// process locks itself down. See docs/aether_compared_to_capsicum.md.
//
// spawn_sandboxed sets AETHER_CAPSICUM=1 in the child's environment when
// the program it launches is itself an Aether binary, so an Aether child
// gets OS-enforced containment without its source asking for it. A
// program can also be launched with the variable set by hand.
//
// Off FreeBSD this is a no-op: capability mode does not exist, and the
// variable is ignored.
//
// Self-guarded so it compiles to an empty object on non-FreeBSD; the
// no-op definition is provided unconditionally so the codegen-emitted
// call always links.

#include <stdio.h>
#include <stdlib.h>

#if defined(__FreeBSD__)

#include <sys/capsicum.h>
#include <unistd.h>      // feature_present
#include <string.h>

void aether_capsicum_autosandbox(void) {
    const char* v = getenv("AETHER_CAPSICUM");
    if (!v || strcmp(v, "1") != 0) {
        return;  // not requested
    }

    // The kernel must actually support Capsicum. If the variable was set
    // but the kernel can't enforce it, fail loud rather than run
    // unconfined — a silent downgrade of a requested sandbox is a
    // security footgun.
    if (!feature_present("security_capabilities")) {
        fprintf(stderr,
            "[aether] AETHER_CAPSICUM=1 but this kernel lacks Capsicum "
            "support; refusing to run unsandboxed\n");
        exit(70);  // EX_SOFTWARE — environment cannot honour the request
    }

    if (cap_enter() != 0) {
        perror("[aether] cap_enter");
        fprintf(stderr,
            "[aether] AETHER_CAPSICUM=1 but cap_enter() failed; refusing "
            "to run unsandboxed\n");
        exit(70);
    }

    // Capability mode is now active and irreversible. From here the
    // program can act only on file descriptors it already holds; any
    // attempt to open a new path or bind a socket fails with ECAPMODE.
    if (getenv("AETHER_CAPSICUM_VERBOSE")) {
        fprintf(stderr, "[aether] entered Capsicum capability mode\n");
    }
}

#else

// Non-FreeBSD: capability mode does not exist. The hook is a no-op so
// the codegen-emitted call links and does nothing.
void aether_capsicum_autosandbox(void) {
    // AETHER_CAPSICUM is intentionally ignored off FreeBSD — there is no
    // OS sandbox to enter. Portable programs probe std.capsicum.available()
    // if they need to know.
}

#endif // __FreeBSD__
