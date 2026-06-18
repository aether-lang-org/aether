// aether_host_factor.c — Embedded Factor Language Host Module
//
// Runs Factor code inside the Aether process via a *forked* libfactor
// that exposes a generic C eval-a-string embedding API. Unlike the other
// hosts, this does NOT bind to a stock distro library: upstream Factor
// ships only `start_standalone_factor` (process takeover), with no
// init/eval-string/result surface. The fork adds
// `factor_embed_eval_oneshot(image_path, src)` — see
// github.com/aether-lang-org/factor-language, branch feat/embed-api,
// vm/embed_api.cpp — and this bridge dlopen's that lib.
//
// LOADING MODEL: dlopen, mirroring contrib/host/lua and friends — the
// embed symbol is resolved via dlsym at first call, so the bridge .a has
// no unresolved Factor symbols at link time and end-user binaries carry
// no DT_NEEDED libfactor.
//
// SANDBOX CAVEAT (important, and unlike the other hosts): Factor's VM
// does its own GC, JIT (a writable+executable code heap), signal handling
// and threads. The LD_PRELOAD libc-interception sandbox the other hosts
// rely on does NOT cleanly contain it. factor_run_sandboxed currently
// installs no in-process checker — it runs Factor and relies on the
// PROCESS-level sandbox (spawn_sandboxed) for isolation, not the per-call
// libc gate. Documented, not hidden.
//
// CURRENT SCOPE: one-shot eval (init VM, evaluate, tear down) — the result
// is printed to stdout by Factor, not captured. A persistent handle with
// captured string results + key/value map interop are the next rungs once
// the Factor-side embed API grows them (the upstream API is being shaped
// so any embedder can add map interop on top; see the Factor branch).

#include "aether_host_factor.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AETHER_HAS_FACTOR

// --- libfactor dlopen table -------------------------------------------------

static void* libfactor_handle = NULL;

static struct {
    // Generic embedding entry point added by the fork (vm/embed_api.cpp).
    int (*factor_embed_eval_oneshot)(const char* image_path, const char* src);
} g_factor;

// Resolved Factor image path (the bootstrapped factor.image). The VM needs
// it; there is no system default the way liblua self-contains. Supplied by
// $AETHER_FACTOR_IMAGE, else NULL (lets the VM try its own default /
// embedded-image lookup).
static const char* g_factor_image = NULL;

static int resolve_factor_symbols(void* h) {
    *(void**)(&g_factor.factor_embed_eval_oneshot) =
        dlsym(h, "factor_embed_eval_oneshot");
    if (!g_factor.factor_embed_eval_oneshot) {
        fprintf(stderr,
            "aether host_factor: libfactor missing symbol "
            "factor_embed_eval_oneshot (needs the embed-api fork).\n");
        return -1;
    }
    return 0;
}

static void* try_dlopen(const char* name) {
    if (!name || !*name) return NULL;
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

// Load libfactor on first use. Two-step, orchestrator-owned probe (same
// posture as the lua bridge — no hardcoded version list):
//   1. $AETHER_FACTOR_SONAME (exact path/soname of the forked libfactor).
//   2. libfactor.so (unversioned, if installed on the host).
// The image path comes from $AETHER_FACTOR_IMAGE.
static int load_libfactor(void) {
    if (libfactor_handle) return 0;

    if (!g_factor_image) g_factor_image = getenv("AETHER_FACTOR_IMAGE");

    const char* env_soname = getenv("AETHER_FACTOR_SONAME");
    void* h = try_dlopen(env_soname);
    if (!h) h = try_dlopen("libfactor.so");
    if (!h) {
        fprintf(stderr,
            "aether host_factor: cannot dlopen libfactor "
            "(tried $AETHER_FACTOR_SONAME=%s, libfactor.so).\n"
            "  Build the embed-api fork's libfactor and set "
            "AETHER_FACTOR_SONAME to it, plus AETHER_FACTOR_IMAGE to the "
            "bootstrapped factor.image.\n  dlerror: %s\n",
            env_soname ? env_soname : "(unset)",
            dlerror() ? dlerror() : "(none)");
        return -1;
    }

    if (resolve_factor_symbols(h) != 0) {
        dlclose(h);
        return -1;
    }
    libfactor_handle = h;
    return 0;
}

int factor_init(void) {
    return load_libfactor();
}

void factor_finalize(void) {
    // The one-shot embed API owns VM lifecycle per call; nothing persistent
    // to tear down here yet. dlclose left to process exit (matches lua).
}

int factor_run(const char* code) {
    if (!code) return -1;
    if (factor_init() != 0) return -1;
    return g_factor.factor_embed_eval_oneshot(g_factor_image, code);
}

int factor_run_sandboxed(void* perms, const char* code) {
    // See the SANDBOX CAVEAT at the top: Factor's VM is not contained by
    // the in-process libc gate. `perms` is accepted for signature parity
    // with the other hosts but isolation must come from the process-level
    // sandbox around this whole program, not a per-call checker swap.
    (void)perms;
    return factor_run(code);
}

#else /* !AETHER_HAS_FACTOR */

int factor_init(void) {
    fprintf(stderr, "aether host_factor: built without Factor support\n");
    return -1;
}
void factor_finalize(void) {}
int factor_run(const char* code) { (void)code; return factor_init(); }
int factor_run_sandboxed(void* perms, const char* code) {
    (void)perms; (void)code; return factor_init();
}

#endif /* AETHER_HAS_FACTOR */
