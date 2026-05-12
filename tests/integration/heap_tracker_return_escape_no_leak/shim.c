/* Shim for the heap_tracker_return_escape_no_leak regression test.
 * Provides `getrusage_max_rss_kb()` so the Aether probe can measure
 * its own RSS before and after a tight loop of heap-accumulator
 * calls. Pre-fix the loop leaks ~770 KB; post-fix it stays flat.
 *
 * Unit normalisation: `ru_maxrss` is in KB on Linux/BSD but in BYTES
 * on macOS. The probe and the function name both treat the value as
 * KB, so we divide by 1024 on Apple platforms to make the cross-
 * platform `growth > 256` (KB) threshold comparison mean the same
 * thing everywhere. Without this normalisation, macOS readings of
 * 32-128 KB of allocator-zone bookkeeping noise compare as 32768-
 * 131072 "KB" and trip the threshold spuriously.
 *
 * Portability: `<sys/resource.h>` is POSIX-only. On Windows the
 * equivalent is `GetProcessMemoryInfo` via `<psapi.h>` — not wired
 * here because (a) the regression closed by this test is in the
 * heap-string-tracker codegen, which runs identically on every
 * platform, and (b) CONTRIBUTING.md §"Coding for portability"
 * pattern #2 prescribes a graceful skip when a per-platform syscall
 * surface isn't reachable. The Aether-side probe treats a -1 return
 * here as "RSS unavailable" and exits with a PASS message rather
 * than asserting the heap-growth bound. */

#if defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)

long getrusage_max_rss_kb(void) {
    /* Sentinel: probe will skip the bench-growth assertion. */
    return -1;
}

#else

#include <sys/resource.h>

long getrusage_max_rss_kb(void) {
    struct rusage r;
    if (getrusage(RUSAGE_SELF, &r) != 0) return -1;
#if defined(__APPLE__) || defined(__MACH__)
    return r.ru_maxrss / 1024;
#else
    return r.ru_maxrss;
#endif
}

#endif
