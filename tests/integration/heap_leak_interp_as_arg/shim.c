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
