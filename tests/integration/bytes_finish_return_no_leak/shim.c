/* Tiny shim for the std.strbuilder no-leak probe.
 *
 * Exposes getrusage_max_rss_kb so the probe can assert RSS stays
 * bounded across a long loop of strbuilder build/finish cycles. The
 * Windows MinGW build doesn't expose this; we return -1 there and let
 * the probe fall back to a smoke-only assertion (codegen path still
 * runs without crashing).
 *
 * IMPORTANT — current RSS, not peak. The name is historical; this
 * reports CURRENT resident set size, not `getrusage`'s `ru_maxrss`.
 * `ru_maxrss` is a monotonic high-water mark, so a tight malloc/free
 * churn loop registers transient allocator peaks as "growth" even
 * when every allocation is freed — flaky noise that overlaps a real
 * leak's magnitude. Current RSS drops when memory is freed, so a
 * leak-free loop returns to baseline (~0 growth) while a real leak
 * shows sustained growth. macOS via mach task_info, Linux/BSD via
 * /proc/self/statm, Windows -1. */
#if defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)
long getrusage_max_rss_kb(void) { return -1; }
#elif defined(__APPLE__) || defined(__MACH__)
#include <mach/mach.h>
long getrusage_max_rss_kb(void) {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS) {
        return -1;
    }
    return (long)(info.resident_size / 1024);  /* bytes -> KB */
}
#else  /* Linux / other POSIX with /proc */
#include <stdio.h>
#include <unistd.h>
long getrusage_max_rss_kb(void) {
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long total_pages = 0, resident_pages = 0;
    if (fscanf(f, "%ld %ld", &total_pages, &resident_pages) != 2) {
        fclose(f);
        return -1;
    }
    fclose(f);
    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    return resident_pages * page_kb;
}
#endif
