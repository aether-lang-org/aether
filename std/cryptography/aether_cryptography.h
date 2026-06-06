/* std.cryptography — cryptographic hash primitives + Base64 codec.
 *
 * Surface (in module.ae's wrapper layer):
 *   sha1_hex(data, length)       — one-shot SHA-1 → lowercase hex
 *   sha256_hex(data, length)     — one-shot SHA-256 → lowercase hex
 *   hash_hex(algo, data, length) — algorithm-by-name dispatcher
 *   hash_supported(algo)         — config-time probe; 1/0 answer
 *   base64_encode(data, length)  — RFC 4648 §4 standard alphabet, unpadded
 *   base64_decode(b64)           — bytes + length via TLS split-accessor
 *
 * HMAC, key derivation, symmetric ciphers, signing, streaming digests,
 * URL-safe Base64 (RFC 4648 §5), and PKCS#7 / PEM parsing are
 * deliberately out of scope. See docs/stdlib-vs-contrib.md for the
 * "one obvious shape" criterion.
 *
 * When the build has AETHER_HAS_OPENSSL (the default on every
 * platform where OpenSSL is available), the implementation is a
 * thin veneer over libcrypto. Without OpenSSL, every entry point
 * returns NULL / 0 so the Go-style Aether wrappers report
 * "openssl unavailable" cleanly.
 */

#ifndef AETHER_CRYPTOGRAPHY_H
#define AETHER_CRYPTOGRAPHY_H

/* Return a newly-allocated, NUL-terminated lowercase-hex digest
 * (40 chars for SHA-1, 64 chars for SHA-256) or NULL on failure.
 * Caller owns the returned buffer and frees it with free().
 *
 * `data` may be an AetherString* or a plain char*; `length` is the
 * explicit byte count (binary-safe, embedded NULs OK). A `length`
 * of 0 hashes the empty string — SHA-256 of "" is a well-defined
 * constant. */
char* cryptography_sha1_hex_raw(const char* data, int length);
char* cryptography_sha256_hex_raw(const char* data, int length);

/* Generic algorithm-by-name digest. `algo` is one of "sha1", "sha256",
 * or any other name OpenSSL's EVP_get_digestbyname() recognizes
 * ("sha384", "sha512", "sha3-256", ...). Returns the same
 * lowercase-hex shape as the per-algorithm functions, or NULL on
 * unknown algorithm / digest failure / no OpenSSL. Caller frees. */
char* cryptography_hash_hex_raw(const char* algo, const char* data, int length);

/* Probe whether the OpenSSL backend in this build can compute `algo`.
 * Returns 1 yes, 0 no. Always succeeds (never errors); callers use
 * this at config time to validate user-supplied algorithm names
 * before they hit hash_hex_raw. Returns 0 when built without
 * OpenSSL. */
int cryptography_hash_supported(const char* algo);

/* MD4 — per-block strong checksum in the .zsync wire format (#637).
 * Bound to EVP_md4() directly rather than via EVP_get_digestbyname()
 * so it works on stock OpenSSL 3 where MD4 lives in the legacy
 * provider that the by-name lookup doesn't consult. Returns
 * lowercase hex (32 chars). On OpenSSL builds without MD4 at all,
 * returns NULL. */
char* cryptography_md4_hex_raw(const char* data, int length);

/* MD5 — Content-MD5 / ETag interop (#631). Same shape and contract
 * as the sha*_hex_raw siblings; the algorithm is documented as legacy
 * in module.ae. Returns lowercase hex (32 chars). */
char* cryptography_md5_hex_raw(const char* data, int length);

/* HMAC-SHA256 (#631). Thin veneer over libcrypto's HMAC(). The
 * raw variant returns the 32-byte digest via the binary-digest TLS
 * split-accessor (see below); the hex variant returns
 * lowercase hex (64 chars) the caller frees. SigV4's key-derivation
 * chain needs the raw form — each round's output keys the next. */
char* cryptography_hmac_sha256_hex_raw(const char* key, int key_len,
                                      const char* msg, int msg_len);
/* Sets the binary-digest TLS slot. Returns 1 on success, 0 on
 * failure / no OpenSSL. Bytes available via
 * cryptography_get_binary_digest() / ..._length(). */
int   cryptography_hmac_sha256_bytes_raw(const char* key, int key_len,
                                         const char* msg, int msg_len);

/* Binary (raw-bytes) digest accessors for length-aware, binary-safe
 * digest output (#637). Same TLS split-accessor shape as base64_decode
 * — the digest lives in a per-thread buffer that remains valid until
 * the next bytes-producing call on the same thread. Aether-side
 * wrappers copy into an AetherString via string_new_with_length.
 *
 *   md4_bytes        → 16-byte MD4 digest
 *   sha1_bytes       → 20-byte SHA-1
 *   sha256_bytes     → 32-byte SHA-256
 *   hash_bytes(algo) → algorithm-by-name (same dispatcher rules as
 *                      hash_hex_raw)
 *   hmac_sha256_bytes (above) shares the same TLS slot.
 *
 * Returns 1 on success, 0 on failure / no OpenSSL / unknown
 * algorithm. */
int cryptography_md4_bytes_raw(const char* data, int length);
int cryptography_md5_bytes_raw(const char* data, int length);
int cryptography_sha1_bytes_raw(const char* data, int length);
int cryptography_sha256_bytes_raw(const char* data, int length);
int cryptography_hash_bytes_raw(const char* algo, const char* data, int length);
const char* cryptography_get_binary_digest(void);
int         cryptography_get_binary_digest_length(void);
void        cryptography_release_binary_digest(void);

/* Base64 encode `length` bytes of `data` using the RFC 4648 §4
 * standard alphabet, unpadded (callers needing `=` padding append
 * `=` themselves to make the length a multiple of 4). Returns a
 * newly-allocated NUL-terminated string the caller frees, or NULL
 * on OOM / no OpenSSL. */
char* cryptography_base64_encode_raw(const char* data, int length);

/* Padded sibling of cryptography_base64_encode_raw — RFC 4648 §4
 * standard alphabet WITH `=` padding to a multiple of 4 bytes. Used
 * by callers whose wire format expects padded base64 (most decoders
 * that aren't RFC-strict; some auth headers; common JSON-encoded
 * blob formats). Same lifetime contract as the unpadded form:
 * caller frees the returned NUL-terminated string. Returns NULL on
 * OOM / no OpenSSL. */
char* cryptography_base64_encode_padded_raw(const char* data, int length);

/* Decode a Base64 string. Returns 1 on success, 0 on failure
 * (malformed input / OOM / no OpenSSL). On success, the decoded
 * bytes live in a thread-local buffer accessible via
 * cryptography_get_base64_decode() and ..._length(); they remain
 * valid until the next call to cryptography_base64_decode_raw()
 * on the same thread. Same TLS split-accessor pattern as
 * std.fs.read_binary. */
int cryptography_base64_decode_raw(const char* b64);
const char* cryptography_get_base64_decode(void);
int cryptography_get_base64_decode_length(void);
void cryptography_release_base64_decode(void);

/* CSPRNG (#630). The cryptographically-secure RNG missing from
 * std.math (which is a seedable PRNG, fine for sampling, NOT for
 * secrets). Backed by the OS CSPRNG on every supported platform:
 *   - Linux:    getrandom(2) → /dev/urandom fallback
 *   - macOS/BSD: arc4random_buf(3)
 *   - Windows:  BCryptGenRandom (CNG)
 *
 * Same TLS split-accessor shape as the binary digests / base64.
 * Independent buffer slot — concurrent random_bytes and digest work
 * on the same thread don't clobber. No OpenSSL dependency: the OS
 * sources are the right choice and don't need libcrypto init. */
int cryptography_random_bytes_raw(int n);
const char* cryptography_get_random_bytes(void);
int         cryptography_get_random_bytes_length(void);
void        cryptography_release_random_bytes(void);

#endif /* AETHER_CRYPTOGRAPHY_H */
