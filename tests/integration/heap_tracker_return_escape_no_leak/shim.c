/* Shim for the heap_tracker_return_escape_no_leak regression test.
 * Provides `getrusage_max_rss_kb()` so the Aether probe can measure
 * its own RSS before and after a tight loop of heap-accumulator
 * calls. Pre-fix the loop leaks ~770 KB; post-fix it stays flat.
 *
 * Note: `ru_maxrss` is in KB on Linux and BYTES on macOS. The probe's
 * threshold is generous enough (256 KB or 256 × 1024 bytes) that
 * either unit lets the test discriminate the post-fix flat-RSS shape
 * from the pre-fix linearly-growing one.
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
    return r.ru_maxrss;
}

#endif
