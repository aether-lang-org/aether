#include "aether_crypto.h"
#include "../../runtime/aether_resource_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
/* arc4random_buf is in stdlib.h on macOS/BSD. */
#else
/* Linux: prefer getrandom(2); fall back to reading /dev/urandom. */
#include <sys/random.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif

/* TLS slot — same shape as the cryptography module's binary-digest
 * and base64-decode slots. Per-thread so concurrent random draws on
 * different threads don't clobber. Lifetime is until the next
 * random_bytes_raw call on the same thread or an explicit release. */
static __thread unsigned char* g_rng_buf = NULL;
static __thread size_t         g_rng_cap = 0;
static __thread int            g_rng_len = 0;

static void release_rng_locked(void) {
    if (g_rng_buf) aether_caps_free(g_rng_buf, g_rng_cap);
    g_rng_buf = NULL;
    g_rng_cap = 0;
    g_rng_len = 0;
}

void crypto_release_random_bytes(void) {
    release_rng_locked();
}

const char* crypto_get_random_bytes(void) {
    return g_rng_buf ? (const char*)g_rng_buf : "";
}

int crypto_get_random_bytes_length(void) {
    return g_rng_len;
}

#ifdef _WIN32
static int fill_random(unsigned char* out, size_t n) {
    NTSTATUS rc = BCryptGenRandom(NULL, out, (ULONG)n,
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return rc == 0 ? 1 : 0;
}
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
static int fill_random(unsigned char* out, size_t n) {
    /* arc4random_buf cannot fail — it abort()s if the kernel can't
     * provide bytes, which is a fatal condition anyway. */
    arc4random_buf(out, n);
    return 1;
}
#else
/* Linux. getrandom(2) (glibc 2.25+, kernel 3.17+) is the right
 * source. The /dev/urandom fallback handles very old kernels (CI
 * matrices, embedded). EINTR retry is required on both paths —
 * a syscall interrupted by a signal returns -1 with errno=EINTR
 * and must be re-tried, not failed. */
static int fill_random(unsigned char* out, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(out + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOSYS) break;  /* fall through to /dev/urandom */
            return 0;
        }
        got += (size_t)r;
    }
    if (got == n) return 1;

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;
    while (got < n) {
        ssize_t r = read(fd, out + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return 0;
        }
        if (r == 0) { close(fd); return 0; }
        got += (size_t)r;
    }
    close(fd);
    return 1;
}
#endif

int crypto_random_bytes_raw(int n) {
    release_rng_locked();
    if (n < 0) return 0;

    /* n == 0 yields a 1-byte buffer with length 0, mirroring the
     * base64_decode("") shape — gives callers a non-NULL accessor
     * pointer they can pass to string_new_with_length(_, 0). */
    size_t alloc = n > 0 ? (size_t)n : 1;
    unsigned char* buf = (unsigned char*)aether_caps_malloc(alloc);
    if (!buf) return 0;

    if (n > 0 && !fill_random(buf, (size_t)n)) {
        aether_caps_free(buf, alloc);
        return 0;
    }
    if (n == 0) buf[0] = 0;

    g_rng_buf = buf;
    g_rng_cap = alloc;
    g_rng_len = n;
    return 1;
}
