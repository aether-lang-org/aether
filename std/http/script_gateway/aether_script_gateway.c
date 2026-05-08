/* aether_script_gateway.c — in-process script gateway (issue #384).
 *
 * Mount-time pipeline:
 *   1. dlopen(so_path, RTLD_NOW | RTLD_LOCAL)
 *   2. dlsym(handle, "aether_script_handle")
 *   3. Allocate a ScriptMount struct holding (path_prefix, fn).
 *   4. Register a middleware on the host server that fires the fn
 *      when req->path starts with path_prefix.
 *
 * Per-request hot path is one strncmp + one indirect call — no
 * fork, no exec, no IPC.
 */

#include "aether_script_gateway.h"
#include "../../net/aether_http_server.h"
#include "../../../runtime/aether_sandbox.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* (handle, kind, message) tuple — same ABI as fs.copy's tuple but
 * named per-module to keep the typedef tags from colliding across
 * .c files. */
typedef struct {
    int _0;            // 1 on success (mount registered) / 0 on failure
    int _1;            // AETHER_SCRIPT_GATEWAY_KIND_*
    const char* _2;    // "" on success, error message on failure
} _aether_sg_tuple;

#if defined(_WIN32)

/* Windows stub. Real LoadLibrary/GetProcAddress support is a
 * follow-up — Windows DLL hosting is structurally similar but the
 * ABI quirks (no RTLD_LOCAL equivalent, dllimport/dllexport mangling,
 * .lib import-libraries) deserve their own commit. */

_aether_sg_tuple script_gateway_mount_so_raw(void* server,
                                             const char* path_prefix,
                                             const char* so_path) {
    (void)server; (void)path_prefix; (void)so_path;
    _aether_sg_tuple out = { 0, AETHER_SCRIPT_GATEWAY_KIND_UNAVAILABLE,
                             "script_gateway: Windows DLL hosting not yet implemented" };
    return out;
}

#else  /* POSIX (Linux, macOS, FreeBSD, ...) */

#include <dlfcn.h>
#include <unistd.h>

/* Aether-side script entrypoint. Matches the standard HttpHandler
 * signature so we can use the C type directly as the dlsym target. */
typedef void (*ScriptEntry)(HttpRequest* req,
                            HttpServerResponse* res,
                            void* user_data);

typedef struct ScriptMount {
    char* prefix;          // path prefix (heap copy; freed only at exit)
    size_t prefix_len;     // strlen(prefix); precomputed for hot-path strncmp
    void* dl_handle;       // dlopen handle (intentionally not dlclose()d)
    ScriptEntry fn;        // resolved entrypoint
} ScriptMount;

/* Middleware thunk: the host server calls this with the per-mount
 * ScriptMount* in `user_data`. Return 1 = continue chain (path
 * didn't match; let other middleware / route dispatch handle the
 * request). Return 0 = short-circuit (we handled it; don't call
 * downstream handlers). */
static int script_gateway_middleware(HttpRequest* req,
                                     HttpServerResponse* res,
                                     void* user_data) {
    ScriptMount* m = (ScriptMount*)user_data;
    if (!m || !m->fn) return 1;
    const char* path = http_request_path(req);
    if (!path) return 1;
    /* Match the prefix. The empty string matches everything (rare
     * but useful as a catch-all gateway). */
    if (m->prefix_len > 0 && strncmp(path, m->prefix, m->prefix_len) != 0) {
        return 1;
    }
    /* Fire the script's handler. We do NOT pass m->dl_handle through
     * user_data — the script gets a NULL ud, same as a hand-written
     * @c_callback HttpHandler that didn't bind state. If a future
     * version wants to pass per-mount state to the script it can
     * extend this thunk; not in scope for v1. */
    m->fn(req, res, NULL);
    return 0;  // short-circuit: we handled the request
}

_aether_sg_tuple script_gateway_mount_so_raw(void* server,
                                             const char* path_prefix,
                                             const char* so_path) {
    if (!server || !path_prefix || !so_path) {
        _aether_sg_tuple out = { 0, AETHER_SCRIPT_GATEWAY_KIND_INVALID,
                                 "null arg" };
        return out;
    }
    /* The `.so` path is read+exec; the host server is locally
     * authoritative on which script files it's willing to mount.
     * Sandbox check: this is essentially a `dlopen` — gate behind
     * fs_read sandbox so a sandboxed Aether host can't bring in
     * arbitrary native code. */
    if (!aether_sandbox_check("fs_read", so_path)) {
        _aether_sg_tuple out = { 0, AETHER_SCRIPT_GATEWAY_KIND_INVALID,
                                 "sandbox: cannot read .so" };
        return out;
    }

    /* Cheap pre-check: if the file isn't there, dlopen's error
     * message is implementation-defined and not always helpful.
     * access(2) gives us a clean KIND_NOT_FOUND surface. */
    if (access(so_path, R_OK) != 0) {
        _aether_sg_tuple out = { 0, AETHER_SCRIPT_GATEWAY_KIND_NOT_FOUND,
                                 "so_path not readable" };
        return out;
    }

    /* RTLD_NOW: bind every symbol at load (fail fast on missing
     * runtime references — better than mysterious aborts on first
     * request). RTLD_LOCAL: don't promote the script's symbols to
     * the host's global namespace. */
    void* h = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        /* dlerror() clears the internal error state on read — calling
         * it twice in a ternary returns the message on the first call
         * and NULL on the second. Capture once. */
        const char* err = dlerror();
        _aether_sg_tuple out = { 0, AETHER_SCRIPT_GATEWAY_KIND_IO,
                                 err ? err : "dlopen failed" };
        return out;
    }

    /* The script's exported entrypoint name is fixed: callers write
     *
     *   @c_callback aether_script_handle(req: ptr, res: ptr, ud: ptr) {
     *       ...
     *   }
     *
     * at the top level of their .ae script and aetherc emits the
     * symbol unmangled. */
    dlerror();  // clear stale error
    void* sym = dlsym(h, "aether_script_handle");
    const char* dl_err = dlerror();
    if (!sym || dl_err) {
        _aether_sg_tuple out = { 0, AETHER_SCRIPT_GATEWAY_KIND_INVALID,
                                 dl_err ? dl_err : "missing aether_script_handle entrypoint" };
        /* dl_handle leaks intentionally (see header docstring); the
         * mount failed, the host can either retry or exit. */
        return out;
    }

    ScriptMount* m = (ScriptMount*)calloc(1, sizeof(ScriptMount));
    if (!m) {
        _aether_sg_tuple out = { 0, AETHER_SCRIPT_GATEWAY_KIND_IO,
                                 "alloc failed" };
        return out;
    }
    m->prefix = strdup(path_prefix);
    if (!m->prefix) {
        free(m);
        _aether_sg_tuple out = { 0, AETHER_SCRIPT_GATEWAY_KIND_IO,
                                 "alloc failed" };
        return out;
    }
    m->prefix_len = strlen(path_prefix);
    m->dl_handle = h;
    m->fn = (ScriptEntry)sym;

    http_server_use_middleware((HttpServer*)server,
                               script_gateway_middleware,
                               m);

    _aether_sg_tuple out = { 1, AETHER_SCRIPT_GATEWAY_KIND_OK, "" };
    return out;
}

#endif  /* _WIN32 */
