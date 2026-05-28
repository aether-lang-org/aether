#include "aether_os.h"
#include "../../runtime/config/aether_optimization_config.h"
#include "../../runtime/aether_sandbox.h"
#include <stdlib.h>

/* Heap-empty sentinel for the position-0 slot on error returns
 * from extern functions annotated `(string @heap, ...)` —
 * specifically os_run_capture_status_raw and
 * os_run_pipe_drain_and_wait_raw. The caller-side heap-string-
 * tracker auto-frees position 0 on function exit; returning a
 * static literal "" would surface as `free((void*)"")` and abort.
 * Allocate a fresh 1-byte buffer so the @heap contract holds
 * uniformly across success and error paths. NULL on OOM is
 * acceptable — `free(NULL)` is a defined no-op. See #420 v2. */
static char* aether_os_empty_heap(void) {
    char* p = (char*)malloc(1);
    if (p) p[0] = '\0';
    return p;
}

// NOTE on aether_argv0 placement: the implementation lives in
// runtime/aether_runtime.c next to the aether_argc / aether_argv it
// reads. aether_os.c cannot reference those variables directly because
// the compiler binary (aetherc) links std/os/aether_os.c but NOT
// runtime/aether_runtime.c, so a hard reference would break the
// compiler link. Keeping the function next to its state fixes that and
// leaves this file focused on shell/exec helpers.

#if !AETHER_HAS_FILESYSTEM
int os_system(const char* c) { (void)c; return -1; }
char* os_exec_raw(const char* c) { (void)c; return NULL; }
char* os_getenv(const char* n) { (void)n; return NULL; }
int os_execv(const char* p, void* a) { (void)p; (void)a; return -1; }
char* os_which(const char* n) { (void)n; return NULL; }
int os_run(const char* p, void* a, void* e) { (void)p; (void)a; (void)e; return -1; }
char* os_run_capture_raw(const char* p, void* a, void* e) { (void)p; (void)a; (void)e; return NULL; }
typedef struct { const char* _0; int _1; const char* _2; } _tuple_string_int_string;
_tuple_string_int_string os_run_capture_status_raw(const char* p, void* a, void* e) {
    (void)p; (void)a; (void)e;
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "os.run_capture unavailable" };
    return out;
}
typedef struct { int _0; int _1; const char* _2; } _tuple_int_int_string;
typedef struct { int _0; const char* _1; } _tuple_int_string;
_tuple_int_int_string os_run_pipe_raw(const char* p, void* a, void* e) {
    (void)p; (void)a; (void)e;
    _tuple_int_int_string out = { -1, -1, "os.run_pipe unavailable" };
    return out;
}
_tuple_int_string os_wait_pid_raw(int pid) {
    (void)pid;
    _tuple_int_string out = { -1, "os.wait_pid unavailable" };
    return out;
}
_tuple_string_int_string os_run_pipe_drain_and_wait_raw(const char* p, void* a, void* e) {
    (void)p; (void)a; (void)e;
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "os.run_pipe_drain_and_wait unavailable" };
    return out;
}
/* ipc_parent_channel_raw — implemented in std/ipc/aether_ipc.c
 * (its no-FS stub returns -1 there). */
char* os_now_utc_iso8601_raw(void) { return NULL; }
int os_getpid_raw(void) { return 0; }
int64_t os_wall_seconds_raw(void) { return 0; }
int os_wall_micros_raw(void) { return 0; }
int64_t os_now_monotonic_ms_raw(void) { return 0; }
int64_t os_now_monotonic_ns_raw(void) { return 0; }
#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

// Forward declarations for the Aether collections API. aether_os.c sits
// below std/collections in the link order, so we can't include the header
// here without a dependency cycle; the prototypes match
// std/collections/aether_collections.h.
extern int list_size(void* list);
extern void* list_get_raw(void* list, int index);

#ifndef _WIN32
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>  /* fcntl, F_GETFD — used by ipc_parent_channel_raw */
#else
#include <windows.h>
#include <wchar.h>
#include <process.h>  /* _getpid */
#endif

// libaether.a list operations — declared extern so this file doesn't have
// to include the collections header (which would create a build cycle).
extern int   list_size(void* list);
extern void* list_get_raw(void* list, int index);

#ifdef _WIN32
// Forward declaration — implementation at the bottom of the file.
// os_execv needs to call into this before the Windows backend block.
static int win_launch(const char* prog, void* argv_list, void* env_list,
                      int capture_stdout,
                      int* out_exit_code, char** out_capture);
#endif

int os_system(const char* cmd) {
    if (!cmd) return -1;
    if (!aether_sandbox_check("exec", cmd)) return -1;
    int status = system(cmd);
    if (status == -1) return -1;
#ifdef _WIN32
    /* Windows's system() already returns the exit code directly. */
    return status;
#else
    /* POSIX system() returns a wait-status word: low byte = signal,
     * high byte = exit code on normal exit. Aether's contract (per
     * docs/stdlib-reference.md) is "returns exit code" — match the
     * shape of os_run's exit-code handling (line ~493): normal
     * termination → WEXITSTATUS, signal → 128 + signum (shell
     * convention), abnormal → -1. Pre-fix, callers had to `>> 8`
     * by hand on POSIX and not on Windows; that's a cross-platform
     * inconsistency the contract didn't promise. */
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#endif
}

char* os_exec_raw(const char* cmd) {
    if (!cmd) return NULL;
    if (!aether_sandbox_check("exec", cmd)) return NULL;

#ifdef _WIN32
    FILE* pipe = _popen(cmd, "r");
#else
    FILE* pipe = popen(cmd, "r");
#endif
    if (!pipe) return NULL;

    size_t capacity = 1024;
    size_t len = 0;
    char* result = (char*)malloc(capacity);
    if (!result) {
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        return NULL;
    }

    // fgets reads up to sizeof(buffer)-1 per call, so a single chunk
    // won't need more than one doubling — but use `while` anyway to
    // stay safe against future buffer-size changes.
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        size_t chunk = strlen(buffer);
        if (len + chunk + 1 > capacity) {
            size_t new_capacity = capacity;
            while (new_capacity < len + chunk + 1) new_capacity *= 2;
            char* new_result = (char*)realloc(result, new_capacity);
            if (!new_result) {
                free(result);
#ifdef _WIN32
                _pclose(pipe);
#else
                pclose(pipe);
#endif
                return NULL;
            }
            result = new_result;
            capacity = new_capacity;
        }
        memcpy(result + len, buffer, chunk);
        len += chunk;
    }

    result[len] = '\0';

#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

char* os_getenv(const char* name) {
    if (!name) return NULL;
    if (!aether_sandbox_check("env", name)) return NULL;
    char* val = getenv(name);
    if (!val) return NULL;
    return strdup(val);
}

/* Format the current UTC time as an ISO-8601 timestamp string:
 * "YYYY-MM-DDThh:mm:ssZ" (20 chars + NUL). Returns a malloc'd
 * strdup. Uses gmtime_r for thread safety. On any clock / format
 * failure returns an empty string — callers that care about the
 * distinction should check for "" vs a valid timestamp shape.
 *
 * Sub-second precision, timezone offsets, and format flags are
 * out of scope for v1 — keep it to the one shape callers wanting
 * a "timestamp this event happened" field reach for. Additive
 * variants (`now_utc_iso8601_ms`, `format_time`) can land without
 * breaking this one. */
char* os_now_utc_iso8601_raw(void) {
    time_t now = time(NULL);
    struct tm tm_buf;
#ifdef _WIN32
    if (gmtime_s(&tm_buf, &now) != 0) return strdup("");
#else
    if (!gmtime_r(&now, &tm_buf)) return strdup("");
#endif
    char buf[32];
    /* "YYYY-MM-DDThh:mm:ssZ" — 20 bytes + NUL, fits buf easily. */
    if (strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm_buf) == 0) {
        return strdup("");
    }
    return strdup(buf);
}

/* Process identifier for the current process. Useful for tmpfile names
 * (`/tmp/myprog.${pid}.tmp`), per-process locks, log prefixes, and
 * stable tagging across forked children. POSIX uses `getpid(2)`;
 * Windows uses `_getpid()` from <process.h>. Returns an int across
 * both platforms — Windows PIDs fit in 32 bits even though the
 * GetCurrentProcessId() type is DWORD. Sandbox-free: this is a
 * pure-information call, not an action. */
int os_getpid_raw(void) {
#ifdef _WIN32
    return (int)_getpid();
#else
    return (int)getpid();
#endif
}

/* Wall-clock time split into whole seconds since the Unix epoch and
 * the sub-second microsecond fraction (0..999999).  A single
 * gettimeofday/filetime read fills a static cache so the two raw
 * accessors are consistent when called back-to-back. struct timeval /
 * FILETIME stay entirely C-side — Aether only ever sees int64/int. */
#ifdef _WIN32
#include <windows.h>
static void aether_os_wall_now(int64_t *psec, int *pusec) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    /* FILETIME: 100-ns ticks since 1601-01-01. Convert to Unix epoch. */
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL;          /* 1601->1970 offset, 100ns units */
    *psec  = (int64_t)(t / 10000000ULL);
    *pusec = (int)((t / 10ULL) % 1000000ULL);
}
#else
#include <sys/time.h>
static void aether_os_wall_now(int64_t *psec, int *pusec) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) { *psec = 0; *pusec = 0; return; }
    *psec  = (int64_t)tv.tv_sec;
    *pusec = (int)tv.tv_usec;
}
#endif

int64_t os_wall_seconds_raw(void) {
    int64_t sec; int usec;
    aether_os_wall_now(&sec, &usec);
    return sec;
}

int os_wall_micros_raw(void) {
    int64_t sec; int usec;
    aether_os_wall_now(&sec, &usec);
    return usec;
}

/* Monotonic clock primitives. Wall-clock time (above) is NTP-jumpable
 * and tied to UTC; useless for animation tick loops, frame-time
 * budgets, or measuring elapsed time across a function call. These
 * use the per-platform monotonic source whose value-domain is opaque
 * (epoch is boot / process-start / arbitrary) but whose *deltas*
 * monotonically track real elapsed time.
 *
 * IMPORTANT: return type is int64_t, NOT C `long`. Aether's `long`
 * lowers to int64_t on every platform, but C `long` is 32-bit on
 * Windows (LLP64). Returning C `long` would truncate the upper 32
 * bits into the Aether 8-byte slot on Windows — the bug PR #562
 * caught and fixed for string_to_long_raw. Same hazard, same fix.
 *
 *   ms — milliseconds since boot/process-start. Wraparound at ~292
 *        million years from epoch; not a real concern.
 *   ns — nanoseconds. Useful for sub-ms timing (animation t-curves,
 *        microbenchmarks). Wraparound at ~292 years; also fine.
 */
#ifndef _WIN32
int64_t os_now_monotonic_ms_raw(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

int64_t os_now_monotonic_ns_raw(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}
#else
/* Windows: QueryPerformanceCounter gives a high-resolution monotonic
 * counter; QueryPerformanceFrequency gives its tick rate. Frequency
 * is invariant across the OS lifetime (Win7+), so we cache it on first
 * call. Both functions are documented to always succeed on those
 * versions, so we treat a 0 frequency as the only failure mode. */
static LARGE_INTEGER qpc_freq_cache = {0};
static int qpc_freq_initialized = 0;

static int64_t qpc_freq_ticks_per_sec(void) {
    if (!qpc_freq_initialized) {
        if (!QueryPerformanceFrequency(&qpc_freq_cache)) return 0;
        qpc_freq_initialized = 1;
    }
    return (int64_t)qpc_freq_cache.QuadPart;
}

int64_t os_now_monotonic_ms_raw(void) {
    int64_t freq = qpc_freq_ticks_per_sec();
    if (freq == 0) return 0;
    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter(&counter)) return 0;
    /* Compute ms as (ticks * 1000) / freq. Doing the multiply first
     * preserves sub-second precision; on a 10 MHz counter this loses
     * resolution at 2^53 ticks / 10^4 ≈ 28e8 seconds (90 years) which
     * is fine. For very-long-lived processes the ns variant has more
     * headroom because freq divides cleanly into 10^9 on x86. */
    return ((int64_t)counter.QuadPart * 1000) / freq;
}

int64_t os_now_monotonic_ns_raw(void) {
    int64_t freq = qpc_freq_ticks_per_sec();
    if (freq == 0) return 0;
    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter(&counter)) return 0;
    /* Split-multiply to avoid overflowing int64 on long uptimes:
     * ticks*1e9/freq could overflow at ~2^53/1e9 ≈ 9e15 ticks (≈ 28
     * years on a 10 MHz counter). The split form: (ticks / freq) * 1e9
     * + ((ticks % freq) * 1e9 / freq) — exact, no overflow. */
    int64_t ticks = (int64_t)counter.QuadPart;
    int64_t whole_sec = ticks / freq;
    int64_t frac_ticks = ticks % freq;
    return whole_sec * 1000000000 + (frac_ticks * 1000000000) / freq;
}
#endif

int os_execv(const char* prog, void* argv_list) {
    if (!prog) return -1;
    if (!aether_sandbox_check("exec", prog)) return -1;

#ifdef _WIN32
    // Windows has no true exec that replaces the process in place. The
    // closest semantic: spawn the child synchronously, inherit stdio,
    // and exit with the child's exit code so the caller sees the same
    // effective behavior as POSIX execvp (the original process is gone
    // after this call returns successfully — but technically returns
    // via exit, not via in-place replacement).
    int exit_code = 0;
    if (win_launch(prog, argv_list, NULL, 0, &exit_code, NULL) != 0) {
        return -1;
    }
    fflush(stdout);
    fflush(stderr);
    exit(exit_code);
    return -1;  // unreachable
#else
    // Build a NULL-terminated char* array from the Aether list. We copy
    // pointers only — the list owns the string storage and keeps it
    // alive for the duration of the call (which, on success, is the
    // rest of forever — the process image is replaced in place).
    int n = argv_list ? list_size(argv_list) : 0;

    // Guard against pathological sizes before multiplying for malloc.
    if (n < 0) return -1;
    if ((size_t)n > (SIZE_MAX / sizeof(char*)) - 2) return -1;

    char** argv = (char**)malloc(sizeof(char*) * (size_t)(n + 2));
    if (!argv) return -1;

    // Canonical POSIX behaviour: argv[0] is the program name. If the
    // caller passed an empty list we synthesise argv[0] from `prog` so
    // the callee sees a sensible value; otherwise the caller owns the
    // whole argv and we trust their layout.
    int ai = 0;
    if (n == 0) {
        argv[ai++] = (char*)prog;
    } else {
        for (int i = 0; i < n; i++) {
            void* item = list_get_raw(argv_list, i);
            if (!item) {
                // Bail early rather than pass NULL into execvp's
                // variadic-argv contract, which has undefined behaviour
                // on many implementations.
                free(argv);
                return -1;
            }
            argv[ai++] = (char*)item;
        }
    }
    argv[ai] = NULL;

    // Flush stdio before replacing the process image. execvp destroys
    // the caller's stdio buffers, so anything println'd but not yet
    // flushed would be silently lost. Line-buffered output already on
    // a terminal is safe, but redirected / pipe output is typically
    // fully-buffered — without this flush, pre-exec diagnostics vanish.
    fflush(stdout);
    fflush(stderr);

    // execvp honours PATH if `prog` does not contain a slash. On
    // success this call never returns; on failure we free scratch and
    // report -1. We intentionally do not touch errno — callers that
    // want diagnostic detail should read it themselves after the call
    // via a dedicated wrapper (not exposed yet).
    execvp(prog, argv);
    free(argv);
    return -1;
#endif
}

// Search PATH for an executable. POSIX semantics:
//   1. If `name` contains a '/', it's treated as a path (absolute or
//      relative to cwd). Return it as-is if executable, else NULL.
//   2. Otherwise iterate through colon-separated entries in $PATH (or a
//      sensible default if PATH isn't set), looking for `<dir>/<name>`
//      that's executable. Return the first hit.
//
// Caller owns the returned string.
char* os_which(const char* name) {
    if (!name || !*name) return NULL;
    if (!aether_sandbox_check("env", "PATH")) return NULL;

#ifdef _WIN32
    // Windows: PATH separator is ';', and executable extensions are
    // enumerated via PATHEXT (e.g. ".COM;.EXE;.BAT;.CMD"). Absolute or
    // relative paths with backslashes or drive letters are returned
    // as-is after existence check.
    size_t name_len = strlen(name);

    // Name already has a path component? (slash, backslash, or drive letter)
    int has_path = 0;
    for (size_t i = 0; i < name_len; i++) {
        if (name[i] == '\\' || name[i] == '/' || (i == 1 && name[i] == ':')) {
            has_path = 1;
            break;
        }
    }

    // Has an extension already?
    const char* dot = strrchr(name, '.');
    const char* last_sep = NULL;
    for (size_t i = 0; i < name_len; i++) {
        if (name[i] == '\\' || name[i] == '/') last_sep = name + i;
    }
    int has_ext = dot && (!last_sep || dot > last_sep);

    const char* pathext = getenv("PATHEXT");
    if (!pathext || !*pathext) pathext = ".COM;.EXE;.BAT;.CMD";

    // Helper: try a candidate path; return strdup of it if the file exists.
    // Inlined via macro since we use it in both the has-path and PATH-search branches.
    #define WIN_WHICH_TRY(candidate) do { \
        DWORD attrs = GetFileAttributesA(candidate); \
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) { \
            return strdup(candidate); \
        } \
    } while (0)

    char buf[MAX_PATH];

    if (has_path) {
        if (has_ext) {
            WIN_WHICH_TRY(name);
            return NULL;
        }
        // Try name with each PATHEXT extension.
        const char* p = pathext;
        while (*p) {
            const char* end = strchr(p, ';');
            size_t ext_len = end ? (size_t)(end - p) : strlen(p);
            if (name_len + ext_len + 1 < sizeof(buf)) {
                memcpy(buf, name, name_len);
                memcpy(buf + name_len, p, ext_len);
                buf[name_len + ext_len] = '\0';
                WIN_WHICH_TRY(buf);
            }
            if (!end) break;
            p = end + 1;
        }
        return NULL;
    }

    const char* path = getenv("PATH");
    if (!path || !*path) return NULL;

    const char* p = path;
    while (*p) {
        const char* end = strchr(p, ';');
        size_t dirlen = end ? (size_t)(end - p) : strlen(p);
        if (dirlen == 0 || dirlen + 1 + name_len >= sizeof(buf)) {
            if (!end) break;
            p = end + 1;
            continue;
        }
        memcpy(buf, p, dirlen);
        buf[dirlen] = '\\';
        memcpy(buf + dirlen + 1, name, name_len);
        buf[dirlen + 1 + name_len] = '\0';

        if (has_ext) {
            WIN_WHICH_TRY(buf);
        } else {
            char ext_buf[MAX_PATH];
            const char* ep = pathext;
            while (*ep) {
                const char* eend = strchr(ep, ';');
                size_t elen = eend ? (size_t)(eend - ep) : strlen(ep);
                size_t base_len = dirlen + 1 + name_len;
                if (base_len + elen + 1 < sizeof(ext_buf)) {
                    memcpy(ext_buf, buf, base_len);
                    memcpy(ext_buf + base_len, ep, elen);
                    ext_buf[base_len + elen] = '\0';
                    WIN_WHICH_TRY(ext_buf);
                }
                if (!eend) break;
                ep = eend + 1;
            }
        }

        if (!end) break;
        p = end + 1;
    }
    #undef WIN_WHICH_TRY
    return NULL;
#else
    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) return strdup(name);
        return NULL;
    }

    const char* path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";

    size_t name_len = strlen(name);
    char buf[4096];
    const char* p = path;
    while (*p) {
        const char* end = strchr(p, ':');
        size_t dirlen = end ? (size_t)(end - p) : strlen(p);
        // Empty entry means current directory (POSIX).
        if (dirlen == 0) {
            // Guard: we write "./" (2 bytes) plus name_len+1 bytes
            // (including null terminator) starting at buf+2. The last
            // written byte is at index (2 + name_len), which must be
            // strictly less than sizeof(buf) for validity.
            if (2 + name_len < sizeof(buf)) {
                buf[0] = '.';
                buf[1] = '/';
                memcpy(buf + 2, name, name_len + 1);
                if (access(buf, X_OK) == 0) return strdup(buf);
            }
        } else if (dirlen + 1 + name_len < sizeof(buf)) {
            memcpy(buf, p, dirlen);
            buf[dirlen] = '/';
            memcpy(buf + dirlen + 1, name, name_len + 1);
            if (access(buf, X_OK) == 0) return strdup(buf);
        }
        if (!end) break;
        p = end + 1;
    }
    return NULL;
#endif
}


// --- os_run / os_run_capture: argv-based child process launch ---
//
// Both functions take an Aether list as the argv (and optional env)
// rather than a shell-string command line. There is no /bin/sh in the
// loop, so paths-with-spaces, $variables, |, ;, *, and other shell
// metacharacters in argv items are passed verbatim. This eliminates a
// large class of quoting bugs and makes the same Aether code portable
// to platforms without a POSIX shell.
//
// Implementation: POSIX uses fork + execvp + waitpid, with execve when
// an explicit env is provided. The Windows backend is a TODO — for now
// it just returns -1 / NULL on _WIN32 builds.

#ifndef _WIN32

// Build a NULL-terminated argv array from an Aether list. The first
// entry in argv[] is `prog`. Caller must free the returned array (the
// strings inside are pointers into the Aether list and must NOT be
// freed individually). Returns NULL on allocation failure.
static char** build_argv_array(const char* prog, void* argv_list) {
    int n = argv_list ? list_size(argv_list) : 0;
    char** av = (char**)malloc(sizeof(char*) * (size_t)(n + 2));
    if (!av) return NULL;
    av[0] = (char*)prog;
    for (int i = 0; i < n; i++) {
        av[i + 1] = (char*)list_get_raw(argv_list, i);
    }
    av[n + 1] = NULL;
    return av;
}

// Build a NULL-terminated environ array from an Aether list of
// "KEY=VALUE" strings. Returns NULL if env_list is NULL (caller should
// inherit parent env in that case). Caller must free the returned
// array.
static char** build_envp_array(void* env_list) {
    if (!env_list) return NULL;
    int n = list_size(env_list);
    char** envp = (char**)malloc(sizeof(char*) * (size_t)(n + 1));
    if (!envp) return NULL;
    for (int i = 0; i < n; i++) {
        envp[i] = (char*)list_get_raw(env_list, i);
    }
    envp[n] = NULL;
    return envp;
}

int os_run(const char* prog, void* argv_list, void* env_list) {
    if (!prog) return -1;
    if (!aether_sandbox_check("exec", prog)) return -1;

    char** av = build_argv_array(prog, argv_list);
    if (!av) return -1;
    char** envp = build_envp_array(env_list);

    pid_t pid = fork();
    if (pid < 0) {
        free(av);
        free(envp);
        return -1;
    }
    if (pid == 0) {
        // Child
        if (envp) {
            execve(prog, av, envp);
        } else {
            execvp(prog, av);
        }
        // exec only returns on failure
        _exit(127);
    }
    // Parent
    free(av);
    free(envp);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

char* os_run_capture_raw(const char* prog, void* argv_list, void* env_list) {
    if (!prog) return NULL;
    if (!aether_sandbox_check("exec", prog)) return NULL;

    int pipefd[2];
    if (pipe(pipefd) != 0) return NULL;

    char** av = build_argv_array(prog, argv_list);
    if (!av) {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }
    char** envp = build_envp_array(env_list);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        free(av);
        free(envp);
        return NULL;
    }
    if (pid == 0) {
        // Child: redirect stdout to pipe write end, close read end
        close(pipefd[0]);
        if (dup2(pipefd[1], 1) < 0) _exit(127);
        close(pipefd[1]);
        if (envp) {
            execve(prog, av, envp);
        } else {
            execvp(prog, av);
        }
        _exit(127);
    }
    // Parent: close write end, read until EOF
    close(pipefd[1]);
    free(av);
    free(envp);

    // Read buffer is page-sized for fewer syscalls on large output;
    // result buffer starts at 4 KB with doubling growth.
    size_t cap = 4096;
    size_t len = 0;
    char* result = (char*)malloc(cap);
    if (!result) {
        close(pipefd[0]);
        // Reap the child so we don't leave a zombie
        int st = 0;
        waitpid(pid, &st, 0);
        return NULL;
    }
    char buf[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            free(result);
            close(pipefd[0]);
            int st = 0;
            waitpid(pid, &st, 0);
            return NULL;
        }
        if (n == 0) break;
        if (len + (size_t)n + 1 > cap) {
            while (len + (size_t)n + 1 > cap) cap *= 2;
            char* bigger = (char*)realloc(result, cap);
            if (!bigger) {
                free(result);
                close(pipefd[0]);
                int st = 0;
                waitpid(pid, &st, 0);
                return NULL;
            }
            result = bigger;
        }
        memcpy(result + len, buf, (size_t)n);
        len += (size_t)n;
    }
    result[len] = '\0';
    close(pipefd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return result;
}

/* Tuple-returning sibling of os_run_capture_raw — captures stdout AND
 * exposes the child's exit code. Returns (stdout, status, err):
 *   - stdout: same shape os_run_capture_raw returns. Empty string
 *             when the spawn fails.
 *   - status: WEXITSTATUS(st) on normal exit, -1 when the child was
 *             killed by a signal or the spawn itself failed.
 *   - err:    "" on successful spawn (regardless of exit code);
 *             non-empty only when the fork/exec couldn't run.
 *
 * This is the canonical entry point for callers that need to
 * distinguish "ran cleanly" from "ran but exited non-zero" (diff3,
 * grep, gcc, etc.). The plain `os_run_capture_raw` stays for callers
 * that don't care about status. Issue #289. */
typedef struct { const char* _0; int _1; const char* _2; } _tuple_string_int_string;

_tuple_string_int_string os_run_capture_status_raw(const char* prog, void* argv_list, void* env_list) {
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "" };
    if (!prog) {
        out._2 = "null prog";
        return out;
    }
    if (!aether_sandbox_check("exec", prog)) {
        out._2 = "denied by sandbox";
        return out;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        out._2 = "pipe failed";
        return out;
    }

    char** av = build_argv_array(prog, argv_list);
    if (!av) {
        close(pipefd[0]);
        close(pipefd[1]);
        out._2 = "argv build failed";
        return out;
    }
    char** envp = build_envp_array(env_list);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        free(av);
        free(envp);
        out._2 = "fork failed";
        return out;
    }
    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], 1) < 0) _exit(127);
        close(pipefd[1]);
        if (envp) execve(prog, av, envp);
        else      execvp(prog, av);
        _exit(127);
    }
    close(pipefd[1]);
    free(av);
    free(envp);

    size_t cap = 4096;
    size_t len = 0;
    char* result = (char*)malloc(cap);
    if (!result) {
        close(pipefd[0]);
        int st = 0;
        waitpid(pid, &st, 0);
        out._2 = "alloc failed";
        return out;
    }
    char buf[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            free(result);
            close(pipefd[0]);
            int st = 0;
            waitpid(pid, &st, 0);
            out._2 = "read failed";
            return out;
        }
        if (n == 0) break;
        if (len + (size_t)n + 1 > cap) {
            while (len + (size_t)n + 1 > cap) cap *= 2;
            char* bigger = (char*)realloc(result, cap);
            if (!bigger) {
                free(result);
                close(pipefd[0]);
                int st = 0;
                waitpid(pid, &st, 0);
                out._2 = "realloc failed";
                return out;
            }
            result = bigger;
        }
        memcpy(result + len, buf, (size_t)n);
        len += (size_t)n;
    }
    result[len] = '\0';
    close(pipefd[0]);

    int st = 0;
    if (waitpid(pid, &st, 0) < 0) {
        /* Free the empty-heap initializer before overwriting with
         * the captured-output buffer (#420 v2 — keeps the @heap
         * contract from leaking 1 byte per failed-waitpid call). */
        free((void*)out._0);
        out._0 = result;
        out._1 = -1;
        out._2 = "waitpid failed";
        return out;
    }
    free((void*)out._0);
    out._0 = result;
    if (WIFEXITED(st)) {
        out._1 = WEXITSTATUS(st);
    } else {
        /* Killed by signal, stopped, or otherwise abnormal — surface
         * as -1 and non-empty err so the caller can distinguish from
         * a real-but-non-zero exit code. */
        out._1 = -1;
        out._2 = "child terminated abnormally";
    }
    return out;
}

/* ============================================================
 * std.ipc — fd-inheritance back-channel for Aether ↔ Aether spawn.
 *
 * Aeb's `aether.driver_test` pattern: parent spawns a child that
 * needs to send back a structured payload (test results, build
 * telemetry, etc) richer than an exit code. Without this, every
 * consumer reinvents the marker-file pattern.
 *
 * Mechanism: parent opens a pipe, dup2's the write end to a chosen
 * fd (3 by convention, but the value goes into AETHER_IPC_FD so
 * the child reads the env var rather than hardcoding the number),
 * forks, exec's. Child can use ipc.parent_channel() to discover
 * the fd, write a payload, close.
 *
 * Three variants of the parent-side spawn:
 *
 *   os_run_pipe_raw()                  — async; returns (read_fd,
 *                                        pid, err). Caller MUST
 *                                        wait via os_wait_pid_raw,
 *                                        and reads concurrently to
 *                                        avoid pipe-buffer deadlock
 *                                        if the payload exceeds
 *                                        OS pipe size (16K macOS,
 *                                        64K Linux).
 *   os_run_pipe_drain_and_wait_raw()   — sync convenience; reads
 *                                        the channel to EOF and
 *                                        waits for child. Same
 *                                        shape as run_capture but
 *                                        captures the IPC fd
 *                                        instead of stdout. No
 *                                        deadlock regardless of
 *                                        payload size.
 *   os_wait_pid_raw()                  — companion to os_run_pipe;
 *                                        reaps child, returns exit.
 *
 * AETHER_IPC_FD is the contract; fd 3 is the default but the env
 * var is what the child reads. Sidesteps fd-allocation surprises
 * in shell intermediaries (e.g. `bash -c '<driver>'`).
 * ============================================================ */

typedef struct { int _0; int _1; const char* _2; } _tuple_int_int_string;
typedef struct { int _0; const char* _1; } _tuple_int_string;

/* Spawn child with a back-channel pipe at fd 3 (write end), set
 * AETHER_IPC_FD=3 in child's env. Returns (parent_read_fd,
 * child_pid, err). On success, err is "" and caller must:
 *   1. Read from parent_read_fd until EOF.
 *   2. Call os_wait_pid_raw(child_pid) to reap.
 *   3. Close parent_read_fd.
 *
 * Failure modes return (-1, -1, "<reason>") with no spawn done.
 *
 * Pipe-buffer deadlock window: if child writes > OS pipe buffer
 * (typically 16K-64K) without parent reading concurrently, the
 * child blocks forever on its write. For payload-at-exit usage
 * with large payloads, prefer os_run_pipe_drain_and_wait_raw. */
_tuple_int_int_string os_run_pipe_raw(const char* prog, void* argv_list, void* env_list) {
    _tuple_int_int_string out = { -1, -1, "" };
    if (!prog) {
        out._2 = "null prog";
        return out;
    }
    if (!aether_sandbox_check("exec", prog)) {
        out._2 = "denied by sandbox";
        return out;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        out._2 = "pipe failed";
        return out;
    }

    char** av = build_argv_array(prog, argv_list);
    if (!av) {
        close(pipefd[0]);
        close(pipefd[1]);
        out._2 = "argv build failed";
        return out;
    }
    char** envp = build_envp_array(env_list);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        free(av);
        free(envp);
        out._2 = "fork failed";
        return out;
    }
    if (pid == 0) {
        /* Child — wire pipefd[1] to fd 3 and tell child where it is. */
        close(pipefd[0]);
        if (dup2(pipefd[1], 3) < 0) _exit(127);
        /* Close the original write end if dup2 didn't already (it
         * does when pipefd[1] != 3; the conditional close is a
         * defensive no-op when dup2 was a self-dup). */
        if (pipefd[1] != 3) close(pipefd[1]);
        /* AETHER_IPC_FD tells the child via env that fd 3 carries
         * the parent channel. The child's ipc.parent_channel()
         * reads this and returns the int — a future change can
         * pick a different fd by setting a different value, no
         * code change needed in the child. */
        setenv("AETHER_IPC_FD", "3", 1);
        if (envp) {
            /* If the caller supplied an explicit envp, AETHER_IPC_FD
             * needs to be present in it — setenv above only updates
             * the current process's environ, but execve will use
             * envp, not environ. Build augmented envp.
             *
             * For now, error if explicit envp is supplied; future
             * work can splice AETHER_IPC_FD into the supplied envp
             * before execve. Most aeb consumers pass null env. */
            _exit(127);
        } else {
            execvp(prog, av);
        }
        _exit(127);
    }
    close(pipefd[1]);
    free(av);
    free(envp);

    out._0 = pipefd[0];
    out._1 = pid;
    return out;
}

/* Wait for a child, return (exit_code, err). Reaps the child so
 * it doesn't become a zombie. Companion to os_run_pipe_raw. */
_tuple_int_string os_wait_pid_raw(int pid) {
    _tuple_int_string out = { -1, "" };
    if (pid <= 0) {
        out._1 = "invalid pid";
        return out;
    }
    int st = 0;
    if (waitpid((pid_t)pid, &st, 0) < 0) {
        out._1 = "waitpid failed";
        return out;
    }
    if (WIFEXITED(st)) {
        out._0 = WEXITSTATUS(st);
    } else {
        out._0 = -1;
        out._1 = "child terminated abnormally";
    }
    return out;
}

/* Convenience: spawn + drain pipe + wait. Reads the back-channel
 * to EOF (driven by the child closing fd 3 or exiting) while the
 * child runs, then waits for the child to exit. Returns the
 * channel payload as a string, the exit code, and an error string.
 *
 * No deadlock risk: the read loop drains continuously, so the
 * pipe buffer never fills regardless of payload size. */
_tuple_string_int_string os_run_pipe_drain_and_wait_raw(const char* prog, void* argv_list, void* env_list) {
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "" };
    /* Delegate to os_run_pipe_raw for the spawn, then drain + wait. */
    _tuple_int_int_string spawn = os_run_pipe_raw(prog, argv_list, env_list);
    if (spawn._2 && spawn._2[0]) {
        out._2 = spawn._2;
        return out;
    }
    int read_fd = spawn._0;
    int pid = spawn._1;

    size_t cap = 4096;
    size_t len = 0;
    char* result = (char*)malloc(cap);
    if (!result) {
        close(read_fd);
        int st = 0;
        waitpid((pid_t)pid, &st, 0);
        out._2 = "alloc failed";
        return out;
    }
    char buf[4096];
    for (;;) {
        ssize_t n = read(read_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            free(result);
            close(read_fd);
            int st = 0;
            waitpid((pid_t)pid, &st, 0);
            out._2 = "read failed";
            return out;
        }
        if (n == 0) break;
        if (len + (size_t)n + 1 > cap) {
            while (len + (size_t)n + 1 > cap) cap *= 2;
            char* bigger = (char*)realloc(result, cap);
            if (!bigger) {
                free(result);
                close(read_fd);
                int st = 0;
                waitpid((pid_t)pid, &st, 0);
                out._2 = "realloc failed";
                return out;
            }
            result = bigger;
        }
        memcpy(result + len, buf, (size_t)n);
        len += (size_t)n;
    }
    result[len] = '\0';
    close(read_fd);

    int st = 0;
    if (waitpid((pid_t)pid, &st, 0) < 0) {
        /* Free the empty-heap initializer before overwriting with
         * the drained payload (#420 v2). */
        free((void*)out._0);
        out._0 = result;
        out._1 = -1;
        out._2 = "waitpid failed";
        return out;
    }
    free((void*)out._0);
    out._0 = result;
    if (WIFEXITED(st)) {
        out._1 = WEXITSTATUS(st);
    } else {
        out._1 = -1;
        out._2 = "child terminated abnormally";
    }
    return out;
}

/* Child side: returns the parent-channel fd if the parent spawned
 * us via os_run_pipe* (i.e. AETHER_IPC_FD is set and the named fd
 * is open writable); -1 otherwise.
 *
 * Implemented in std/ipc/aether_ipc.c — declared here only so the
 * Aether module.ae's `extern ipc_parent_channel_raw()` resolves
 * during link. */

#else // _WIN32

// -----------------------------------------------------------------
// Windows process-exec backend: CreateProcessW with argv-style launch.
//
// The POSIX branch uses execvp directly. On Windows, CreateProcessW
// takes a single UTF-16 command-line string with the very particular
// escaping rules consumed by the C runtime's argv parser. The helpers
// below build that string correctly — the classic "Everyone quotes
// command line arguments the wrong way" rules are applied verbatim.
// -----------------------------------------------------------------

// UTF-8 → UTF-16. Caller frees. Returns NULL on conversion failure.
static wchar_t* utf8_to_wide(const char* utf8) {
    if (!utf8) return NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t* wide = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)wlen);
    if (!wide) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, wlen) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

// UTF-16 → UTF-8. Caller frees. Returns NULL on conversion failure.
static char* wide_to_utf8(const wchar_t* wide) {
    if (!wide) return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return NULL;
    char* utf8 = (char*)malloc((size_t)len);
    if (!utf8) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, len, NULL, NULL) <= 0) {
        free(utf8);
        return NULL;
    }
    return utf8;
}

// Simple growing wide-string buffer for assembling a command line.
typedef struct {
    wchar_t* data;
    size_t   len;
    size_t   cap;
    int      oom;
} WBuf;

static void wbuf_init(WBuf* b) { b->data = NULL; b->len = 0; b->cap = 0; b->oom = 0; }

static int wbuf_reserve(WBuf* b, size_t extra) {
    if (b->oom) return 0;
    size_t need = b->len + extra + 1;
    if (need > b->cap) {
        size_t new_cap = b->cap ? b->cap * 2 : 256;
        while (new_cap < need) new_cap *= 2;
        wchar_t* nd = (wchar_t*)realloc(b->data, sizeof(wchar_t) * new_cap);
        if (!nd) { b->oom = 1; return 0; }
        b->data = nd;
        b->cap = new_cap;
    }
    return 1;
}

static void wbuf_append(WBuf* b, const wchar_t* s) {
    if (!s) return;
    size_t slen = wcslen(s);
    if (!wbuf_reserve(b, slen)) return;
    memcpy(b->data + b->len, s, sizeof(wchar_t) * slen);
    b->len += slen;
    b->data[b->len] = L'\0';
}

static void wbuf_append_char(WBuf* b, wchar_t c) {
    if (!wbuf_reserve(b, 1)) return;
    b->data[b->len++] = c;
    b->data[b->len] = L'\0';
}

// Escape and append `arg` for CRT argv parsing. Rules from MSFT:
// https://learn.microsoft.com/en-us/archive/blogs/twistylittlepassagesallalike/everyone-quotes-command-line-arguments-the-wrong-way
//   - If the arg contains no whitespace and no '"', append verbatim.
//   - Otherwise wrap in quotes. Inside the quotes:
//       * Any run of N backslashes followed by a '"' becomes 2N+1
//         backslashes followed by \".
//       * Any run of N backslashes at the very end of the arg (right
//         before the closing quote) becomes 2N backslashes.
//       * Other backslashes are passed through verbatim.
static void append_escaped_arg(WBuf* b, const wchar_t* arg) {
    int needs_quote = 0;
    if (*arg == L'\0') {
        needs_quote = 1;
    } else {
        for (const wchar_t* p = arg; *p; p++) {
            if (*p == L' ' || *p == L'\t' || *p == L'"') {
                needs_quote = 1;
                break;
            }
        }
    }

    if (!needs_quote) {
        wbuf_append(b, arg);
        return;
    }

    wbuf_append_char(b, L'"');

    const wchar_t* p = arg;
    while (*p) {
        size_t backslashes = 0;
        while (*p == L'\\') { backslashes++; p++; }

        if (*p == L'\0') {
            // Trailing backslashes before closing quote — double them.
            for (size_t i = 0; i < backslashes * 2; i++) wbuf_append_char(b, L'\\');
            break;
        } else if (*p == L'"') {
            // N backslashes + '"' → 2N+1 backslashes + \"
            for (size_t i = 0; i < backslashes * 2 + 1; i++) wbuf_append_char(b, L'\\');
            wbuf_append_char(b, L'"');
            p++;
        } else {
            for (size_t i = 0; i < backslashes; i++) wbuf_append_char(b, L'\\');
            wbuf_append_char(b, *p);
            p++;
        }
    }

    wbuf_append_char(b, L'"');
}

// Build the command-line string for CreateProcessW. argv[0] is the
// program name — we use `prog` if the caller's list is empty, else the
// first list entry. Caller frees.
static wchar_t* build_command_line(const char* prog, void* argv_list) {
    WBuf b; wbuf_init(&b);

    int n = argv_list ? list_size(argv_list) : 0;

    const char* arg0_utf8 = (n > 0) ? (const char*)list_get_raw(argv_list, 0) : prog;
    if (!arg0_utf8) arg0_utf8 = prog;
    wchar_t* warg0 = utf8_to_wide(arg0_utf8);
    if (!warg0) { free(b.data); return NULL; }
    append_escaped_arg(&b, warg0);
    free(warg0);

    int start_index = (n > 0) ? 1 : 0;
    for (int i = start_index; i < n; i++) {
        const char* item = (const char*)list_get_raw(argv_list, i);
        if (!item) continue;
        wchar_t* w = utf8_to_wide(item);
        if (!w) { free(b.data); return NULL; }
        wbuf_append_char(&b, L' ');
        append_escaped_arg(&b, w);
        free(w);
    }

    if (b.oom) { free(b.data); return NULL; }
    return b.data;
}

// Build a UTF-16 environment block from a list of "KEY=VALUE" strings.
// Format: key1=val1\0key2=val2\0\0. Returns NULL if env_list is NULL
// (inherit parent env) or on allocation failure.
static wchar_t* build_environ_block(void* env_list) {
    if (!env_list) return NULL;
    int n = list_size(env_list);
    WBuf b; wbuf_init(&b);

    for (int i = 0; i < n; i++) {
        const char* item = (const char*)list_get_raw(env_list, i);
        if (!item) continue;
        wchar_t* w = utf8_to_wide(item);
        if (!w) { free(b.data); return NULL; }
        wbuf_append(&b, w);
        wbuf_append_char(&b, L'\0');
        free(w);
    }
    wbuf_append_char(&b, L'\0');
    if (b.oom) { free(b.data); return NULL; }
    return b.data;
}

// Shared launch path. If `capture_stdout` is non-zero we redirect the
// child's stdout to a pipe and read it to completion. `out_exit_code`
// and `out_capture` are optional outputs.
static int win_launch(const char* prog, void* argv_list, void* env_list,
                      int capture_stdout,
                      int* out_exit_code, char** out_capture) {
    wchar_t* cmdline = build_command_line(prog, argv_list);
    if (!cmdline) return -1;

    wchar_t* wprog = utf8_to_wide(prog);
    if (!wprog) { free(cmdline); return -1; }

    wchar_t* wenv = build_environ_block(env_list);
    // NULL from build_environ_block when env_list is NULL means "inherit" —
    // that's the correct semantic, not an error.

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);

    HANDLE read_pipe = NULL, write_pipe = NULL;
    SECURITY_ATTRIBUTES sa;

    if (capture_stdout) {
        memset(&sa, 0, sizeof(sa));
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = NULL;
        if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
            free(cmdline); free(wprog); free(wenv);
            return -1;
        }
        // The read end must NOT be inherited by the child.
        SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = write_pipe;
        si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    }

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    BOOL ok = CreateProcessW(
        wprog,         // application name
        cmdline,       // command line (modifiable — CreateProcessW may write)
        NULL, NULL,
        capture_stdout ? TRUE : FALSE,  // inherit handles only when capturing
        flags,
        wenv,          // NULL = inherit parent env
        NULL,          // CWD = current
        &si,
        &pi);

    free(cmdline);
    free(wprog);
    free(wenv);

    if (capture_stdout) {
        // Parent closes the write end so EOF happens when the child exits.
        CloseHandle(write_pipe);
    }

    if (!ok) {
        if (capture_stdout) CloseHandle(read_pipe);
        return -1;
    }

    char* capture_buf = NULL;
    size_t capture_len = 0;

    if (capture_stdout) {
        size_t cap = 1024;
        capture_buf = (char*)malloc(cap);
        if (!capture_buf) {
            CloseHandle(read_pipe);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            return -1;
        }
        char tmp[1024];
        DWORD got = 0;
        while (ReadFile(read_pipe, tmp, sizeof(tmp), &got, NULL) && got > 0) {
            if (capture_len + got + 1 > cap) {
                while (capture_len + got + 1 > cap) cap *= 2;
                char* bigger = (char*)realloc(capture_buf, cap);
                if (!bigger) {
                    free(capture_buf);
                    CloseHandle(read_pipe);
                    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
                    return -1;
                }
                capture_buf = bigger;
            }
            memcpy(capture_buf + capture_len, tmp, got);
            capture_len += got;
        }
        capture_buf[capture_len] = '\0';
        CloseHandle(read_pipe);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (out_exit_code) *out_exit_code = (int)exit_code;
    if (out_capture) *out_capture = capture_buf;
    else free(capture_buf);

    return 0;
}

int os_run(const char* prog, void* argv_list, void* env_list) {
    if (!prog) return -1;
    if (!aether_sandbox_check("exec", prog)) return -1;

    int exit_code = 0;
    if (win_launch(prog, argv_list, env_list, 0, &exit_code, NULL) != 0) {
        return -1;
    }
    return exit_code;
}

char* os_run_capture_raw(const char* prog, void* argv_list, void* env_list) {
    if (!prog) return NULL;
    if (!aether_sandbox_check("exec", prog)) return NULL;

    char* capture = NULL;
    int exit_code = 0;
    if (win_launch(prog, argv_list, env_list, 1, &exit_code, &capture) != 0) {
        free(capture);
        return NULL;
    }
    // Caller conventions: on non-zero exit the POSIX branch still
    // returns the captured stdout so the caller can inspect it.
    (void)exit_code;
    return capture ? capture : strdup("");
}

/* Tuple sibling — exposes exit status. See the POSIX branch for the
 * full contract. Issue #289. */
typedef struct { const char* _0; int _1; const char* _2; } _tuple_string_int_string;

_tuple_string_int_string os_run_capture_status_raw(const char* prog, void* argv_list, void* env_list) {
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "" };
    if (!prog) { out._2 = "null prog"; return out; }
    if (!aether_sandbox_check("exec", prog)) { out._2 = "denied by sandbox"; return out; }

    char* capture = NULL;
    int exit_code = 0;
    if (win_launch(prog, argv_list, env_list, 1, &exit_code, &capture) != 0) {
        free(capture);
        out._2 = "spawn failed";
        return out;
    }
    /* Free the empty-heap initializer before overwriting, so the
     * success path doesn't leak a 1-byte allocation per call. */
    free((void*)out._0);
    out._0 = capture ? capture : aether_os_empty_heap();
    out._1 = exit_code;
    /* err stays "" — Win32 win_launch already returned 0 here, so
     * the spawn itself succeeded. */
    return out;
}

/* std.ipc on Windows: stubs returning unsupported.
 *
 * Windows handle inheritance via STARTUPINFOEX +
 * PROC_THREAD_ATTRIBUTE_HANDLE_LIST is the right approach for a
 * future port, but mapping "child sees this at fd 3" requires
 * coordinated _open_osfhandle on both sides plus careful handling
 * of which std handles get inherited. v1 ships POSIX-only;
 * Windows consumers fall back to the file-marker pattern. */

typedef struct { int _0; int _1; const char* _2; } _tuple_int_int_string;
typedef struct { int _0; const char* _1; } _tuple_int_string;

_tuple_int_int_string os_run_pipe_raw(const char* prog, void* argv_list, void* env_list) {
    (void)prog; (void)argv_list; (void)env_list;
    _tuple_int_int_string out = { -1, -1, "unsupported on Windows" };
    return out;
}

_tuple_int_string os_wait_pid_raw(int pid) {
    (void)pid;
    _tuple_int_string out = { -1, "unsupported on Windows" };
    return out;
}

_tuple_string_int_string os_run_pipe_drain_and_wait_raw(const char* prog, void* argv_list, void* env_list) {
    (void)prog; (void)argv_list; (void)env_list;
    _tuple_string_int_string out = { aether_os_empty_heap(), -1, "unsupported on Windows" };
    return out;
}

/* ipc_parent_channel_raw on Windows — implemented in
 * std/ipc/aether_ipc.c (returns -1 unconditionally; Windows is
 * stub-first per the v1 design). */

#endif // !_WIN32


#endif // AETHER_HAS_FILESYSTEM
