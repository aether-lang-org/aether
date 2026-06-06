/* getrusage_max_rss_kb — current resident-set size in KB, after
 * forcing the allocator to release cached free pages.
 *
 * Despite the historical name, this returns CURRENT RSS, not the
 * peak. The probes compare RSS before/after a long loop to assert a
 * leak doesn't grow the live set. Two macOS hazards make a naive
 * reading flaky:
 *   1. `ru_maxrss` is the PEAK high-water-mark (climbs in allocator-
 *      region chunks, never falls) and is in BYTES on macOS vs KB on
 *      Linux/BSD — wrong metric entirely.
 *   2. Even CURRENT RSS (mach resident_size) counts pages the malloc
 *      zone has freed internally but not yet returned to the OS. Under
 *      concurrent full-suite memory pressure those retained pages
 *      spike by ~1 MB, tripping a tight threshold on a leak-free run
 *      (confirmed: `leaks` reports 0 yet RSS jumps).
 * Fix: call `malloc_zone_pressure_relief(.., 0)` to force the default
 * zone to hand cached free memory back to the OS BEFORE sampling, so
 * resident_size reflects the true live set deterministically. Then
 * the before/after delta is allocator-noise-free. (Linux keeps
 * ru_maxrss; Valgrind/ASan-LSan are its precise gate. macOS also has
 * the deterministic `leaks(1)` gate — see tests/run_macos_leaks.sh.) */
#if defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)
long getrusage_max_rss_kb(void) { return -1; }
#elif defined(__APPLE__) || defined(__MACH__)
#include <mach/mach.h>
#include <malloc/malloc.h>
long getrusage_max_rss_kb(void) {
    /* Return malloc-zone-cached free pages to the OS so resident_size
     * measures the live set, not the allocator's high-water cache. */
    malloc_zone_pressure_relief(malloc_default_zone(), 0);
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS) {
        return -1;
    }
    return (long)(info.resident_size / 1024);  /* bytes → KB */
}
#else
#include <sys/resource.h>
long getrusage_max_rss_kb(void) {
    struct rusage r;
    if (getrusage(RUSAGE_SELF, &r) != 0) return -1;
    return r.ru_maxrss;  /* already KB on Linux/BSD */
}
#endif
