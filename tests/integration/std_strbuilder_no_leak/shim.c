/* Tiny shim for the std.strbuilder no-leak probe.
 *
 * Exposes getrusage_max_rss_kb so the probe can assert RSS stays
 * bounded across a long loop of strbuilder build/finish cycles. The
 * Windows MinGW build doesn't expose getrusage; we return -1 there
 * and let the probe fall back to a smoke-only assertion (codegen
 * path still runs without crashing). */
#if defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)
long getrusage_max_rss_kb(void) { return -1; }
#else
#include <sys/resource.h>
long getrusage_max_rss_kb(void) {
    struct rusage r;
    if (getrusage(RUSAGE_SELF, &r) != 0) return -1;
    return r.ru_maxrss;
}
#endif
