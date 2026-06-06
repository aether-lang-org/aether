/* std.crypto — cryptographically-secure RNG.
 *
 * The CSPRNG primitive missing from std.math (which is a seedable
 * PRNG, fine for sampling, NOT for secrets — see #630).
 *
 * Surface (in module.ae's wrapper layer):
 *   crypto.random_bytes(n) -> (bytes, length, err)
 *
 * Backed by the OS CSPRNG:
 *   - Linux:   getrandom(2) (since 3.17) → /dev/urandom fallback
 *   - macOS:   arc4random_buf(3)
 *   - *BSD:    arc4random_buf(3)
 *   - Windows: BCryptGenRandom (CNG) via BCRYPT_USE_SYSTEM_PREFERRED_RNG
 *
 * No OpenSSL dependency: the OS-provided CSPRNGs are the right
 * source on every supported platform, and they don't need TLS
 * initialization or a library handle. The runtime cost is one
 * syscall per call (getrandom blocks only on a fresh, never-seeded
 * kernel pool, which doesn't happen during a normal boot). Buffer
 * the result if you need many small draws — the syscall overhead
 * dominates for asks under ~64 bytes.
 */

#ifndef AETHER_CRYPTO_H
#define AETHER_CRYPTO_H

#include <stddef.h>

/* Fill the binary-bytes TLS slot with `n` bytes from the OS CSPRNG.
 * Returns 1 on success, 0 on failure (n < 0, allocation failure,
 * syscall failure). Bytes available via crypto_get_random_bytes()
 * + crypto_get_random_bytes_length(). Same per-thread split-accessor
 * shape as std.fs.read_binary / std.cryptography.base64_decode.
 *
 * `n == 0` is allowed — yields a zero-length buffer (length 0,
 * non-NULL accessor pointer for "decoded empty" semantics). */
int crypto_random_bytes_raw(int n);
const char* crypto_get_random_bytes(void);
int         crypto_get_random_bytes_length(void);
void        crypto_release_random_bytes(void);

#endif /* AETHER_CRYPTO_H */
