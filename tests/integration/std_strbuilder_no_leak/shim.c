/* Tiny shim for the std.strbuilder no-leak probe.
 *
 * Exposes getrusage_max_rss_kb so the probe can assert RSS stays
 * bounded across a long loop of strbuilder build/finish cycles. The
 * Windows MinGW build doesn't expose getrusage; we return -1 there
 * and let the probe fall back to a smoke-only assertion (codegen
 * path still runs without crashing).
 *
 * Unit gotcha: getrusage's `ru_maxrss` field is in KILOBYTES on
 * Linux and BSDs but in BYTES on macOS/Darwin. Without the
 * conversion below, the macOS run reads back ~1000× the value the
 * probe's threshold was tuned against and we get spurious failures
 * like "RSS grew 98304 KB" when the actual growth is 96 KB. We
 * normalise to KB so the probe's threshold means the same thing on
 * every platform. */
#if defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)
long getrusage_max_rss_kb(void) { return -1; }
#else
#include <sys/resource.h>
long getrusage_max_rss_kb(void) {
    struct rusage r;
    if (getrusage(RUSAGE_SELF, &r) != 0) return -1;
#if defined(__APPLE__)
    /* Darwin reports bytes; convert to KB to match Linux/BSD. */
    return (long)(r.ru_maxrss / 1024);
#else
    /* Linux and BSD: already in KB per POSIX. */
    return (long)r.ru_maxrss;
#endif
}
#endif
