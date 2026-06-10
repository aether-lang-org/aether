/* Shim for the heap_tracker_return_escape_no_leak regression test.
 * Provides `getrusage_max_rss_kb()` so the Aether probe can measure
 * its own RSS before and after a tight loop of heap-accumulator
 * calls. Pre-fix the loop leaks ~770 KB; post-fix it stays flat.
 *
 * IMPORTANT — current RSS, not peak. The name is historical; this
 * reports CURRENT resident set size, not `getrusage`'s `ru_maxrss`.
 * `ru_maxrss` is a monotonic high-water mark: under a tight
 * malloc/free churn loop it captures transient allocator peaks (and
 * pages the allocator keeps mapped after free), so a leak-free run
 * can still show hundreds of KB of "growth" — run-to-run noise that
 * overlaps a real leak's magnitude and makes the bound flaky. Current
 * RSS, by contrast, DROPS when memory is freed: with the heap-string
 * tracker reclaiming each accumulator, post-loop residency returns to
 * baseline (~0 KB growth), while a genuine return-escape leak shows as
 * sustained growth proportional to the leaked bytes. So the bound
 * discriminates leak from noise on every allocator.
 *
 * Portability: macOS via mach task_info, Linux/BSD via /proc/self/statm,
 * Windows returns the -1 sentinel (the probe then skips the growth
 * assertion — the heap-string-tracker codegen under test runs
 * identically on every platform; CONTRIBUTING.md §"Coding for
 * portability" pattern #2). */

#if defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)

long getrusage_max_rss_kb(void) {
    /* Sentinel: probe will skip the bench-growth assertion. */
    return -1;
}

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
