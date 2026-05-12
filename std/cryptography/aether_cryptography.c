#include "aether_cryptography.h"
#include "../string/aether_string.h"
#include "../../runtime/aether_resource_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AETHER_HAS_OPENSSL
#include <openssl/evp.h>
#endif

/* Unwrap the payload from a `data` argument that may be either an
 * AetherString* or a plain char*. Mirrors the helper in
 * std/fs/aether_fs.c — when callers pass a length-aware AetherString
 * (e.g. from fs.read_binary), the raw pointer is the struct, not
 * the bytes. Without this dispatch, we'd hash the struct header. */
static inline const unsigned char* cryptography_unwrap_bytes(const char* data, int length, size_t* out_len) {
    if (!data) { *out_len = 0; return NULL; }
    if (is_aether_string(data)) {
        const AetherString* s = (const AetherString*)data;
        *out_len = (length >= 0) ? (size_t)length : s->length;
        return (const unsigned char*)s->data;
    }
    *out_len = (length >= 0) ? (size_t)length : strlen(data);
    return (const unsigned char*)data;
}

static char* hex_encode(const unsigned char* digest, size_t digest_len) {
    /* Two hex chars per byte + trailing NUL. */
    char* hex = (char*)malloc(digest_len * 2 + 1);
    if (!hex) return NULL;
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < digest_len; i++) {
        hex[i * 2]     = HEX[(digest[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = HEX[digest[i] & 0x0F];
    }
    hex[digest_len * 2] = '\0';
    return hex;
}

#ifdef AETHER_HAS_OPENSSL
static char* sha_hex(const EVP_MD* md, const char* data, int length) {
    if (length < 0) return NULL;
    size_t want;
    const unsigned char* bytes = cryptography_unwrap_bytes(data, length, &want);
    if (want > 0 && !bytes) return NULL;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return NULL;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    if (EVP_DigestInit_ex(ctx, md, NULL) != 1 ||
        (want > 0 && EVP_DigestUpdate(ctx, bytes, want) != 1) ||
        EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return NULL;
    }
    EVP_MD_CTX_free(ctx);

    return hex_encode(digest, (size_t)digest_len);
}

char* cryptography_sha1_hex_raw(const char* data, int length) {
    return sha_hex(EVP_sha1(), data, length);
}

char* cryptography_sha256_hex_raw(const char* data, int length) {
    return sha_hex(EVP_sha256(), data, length);
}

/* Algorithm-by-name dispatcher. EVP_get_digestbyname returns NULL for
 * unrecognized names; we let that NULL propagate through sha_hex's
 * NULL check. The set of names libcrypto recognizes is broader than
 * what this stdlib documents — sha384, sha512, sha3-256, etc. all
 * work — but the wrapper layer's docs only commit to sha1 + sha256
 * as supported names. Future stdlib additions (per-algo hex helpers)
 * land here too. */
char* cryptography_hash_hex_raw(const char* algo, const char* data, int length) {
    if (!algo) return NULL;
    const EVP_MD* md = EVP_get_digestbyname(algo);
    if (!md) return NULL;
    return sha_hex(md, data, length);
}

int cryptography_hash_supported(const char* algo) {
    if (!algo) return 0;
    return EVP_get_digestbyname(algo) != NULL ? 1 : 0;
}

/* ---- Base64 ----
 *
 * Standard alphabet (RFC 4648 §4), unpadded. OpenSSL's EVP_EncodeBlock
 * pads the output with '=' to a multiple of 4 — we strip the trailing
 * pad bytes after the call to satisfy the unpadded-output contract.
 * EVP_DecodeBlock handles padded-or-unpadded input, but it always
 * decodes in 4-byte groups: an unpadded input length must be padded
 * up to a multiple of 4 with '=' before passing to libcrypto, then
 * the trailing zero bytes from the decoded buffer get trimmed. */

char* cryptography_base64_encode_raw(const char* data, int length) {
    if (length < 0) return NULL;
    size_t want;
    const unsigned char* bytes = cryptography_unwrap_bytes(data, length, &want);
    if (want > 0 && !bytes) return NULL;

    /* EVP_EncodeBlock writes ((n+2)/3)*4 bytes plus a NUL.
     * Cap-aware (#343): the input length is caller-supplied and can
     * be plugin-host-controlled in --emit=lib scenarios. Gate the
     * allocation. The caller frees with plain `free()` per the
     * caller-owned-return contract (aether_resource_caps.h:89-94);
     * the counter drifts up on these paths, same as string_concat. */
    size_t out_cap = ((want + 2) / 3) * 4 + 1;
    char* out = (char*)aether_caps_malloc(out_cap);
    if (!out) return NULL;

    int written = EVP_EncodeBlock((unsigned char*)out, bytes, (int)want);
    if (written < 0) { aether_caps_free(out, out_cap); return NULL; }
    out[written] = '\0';

    /* Strip trailing '=' padding for the unpadded contract. */
    while (written > 0 && out[written - 1] == '=') {
        out[--written] = '\0';
    }
    return out;
}

/* Padded sibling — RFC 4648 §4 standard alphabet WITH `=` padding to
 * a multiple of 4 bytes. Used by callers whose wire format expects
 * padded base64 (most decoders that aren't RFC-strict; some auth
 * headers; common JSON-encoded blob formats). The output is exactly
 * what EVP_EncodeBlock produces — same allocation cost, just no
 * trailing-`=` strip. */
char* cryptography_base64_encode_padded_raw(const char* data, int length) {
    if (length < 0) return NULL;
    size_t want;
    const unsigned char* bytes = cryptography_unwrap_bytes(data, length, &want);
    if (want > 0 && !bytes) return NULL;

    /* Cap-aware (#343): same gating rationale as the unpadded
     * sibling above. */
    size_t out_cap = ((want + 2) / 3) * 4 + 1;
    char* out = (char*)aether_caps_malloc(out_cap);
    if (!out) return NULL;

    int written = EVP_EncodeBlock((unsigned char*)out, bytes, (int)want);
    if (written < 0) { aether_caps_free(out, out_cap); return NULL; }
    out[written] = '\0';
    return out;
}

/* TLS-owned decode buffer + length, mirroring std.fs.read_binary's
 * split-accessor shape. Tracked per-thread so concurrent decodes on
 * different threads don't clobber each other; lifetime is until the
 * next call on the same thread. Aether-side wrappers are expected to
 * copy the bytes out via string_new_with_length before calling
 * back into the C side. Cap tracked alongside (#343) so the release
 * path correctly decrements the cap counter rather than drifting. */
static __thread unsigned char* g_b64_buf = NULL;
static __thread size_t         g_b64_cap = 0;
static __thread int            g_b64_len = 0;

static void release_b64_locked(void) {
    if (g_b64_buf) aether_caps_free(g_b64_buf, g_b64_cap);
    g_b64_buf = NULL;
    g_b64_cap = 0;
    g_b64_len = 0;
}

void cryptography_release_base64_decode(void) {
    release_b64_locked();
}

int cryptography_base64_decode_raw(const char* b64) {
    release_b64_locked();
    if (!b64) return 0;

    size_t in_len;
    const unsigned char* in = cryptography_unwrap_bytes(b64, -1, &in_len);
    if (in_len == 0) {
        /* Decoding "" is a valid request — yields zero bytes. Allocate
         * a 1-byte buffer so the caller can distinguish "decoded 0
         * bytes" from "no data" via the length accessor. */
        g_b64_buf = (unsigned char*)aether_caps_malloc(1);
        if (!g_b64_buf) return 0;
        g_b64_cap = 1;
        g_b64_buf[0] = 0;
        g_b64_len = 0;
        return 1;
    }

    /* EVP_DecodeBlock requires the input length to be a multiple of
     * 4. Pad up with '=' if the caller passed an unpadded string.
     * Cap-aware (#343): input length is untrusted in plugin-host
     * scenarios; both the pad-up buffer and the decode-output buffer
     * are gated and freed through the caps allocator. */
    size_t padded_len = (in_len + 3) / 4 * 4;
    unsigned char* padded = (unsigned char*)aether_caps_malloc(padded_len);
    if (!padded) return 0;
    memcpy(padded, in, in_len);
    for (size_t i = in_len; i < padded_len; i++) padded[i] = '=';

    /* Output is at most 3/4 of input. */
    size_t out_cap = (padded_len / 4) * 3;
    size_t out_alloc = out_cap > 0 ? out_cap : 1;
    unsigned char* out = (unsigned char*)aether_caps_malloc(out_alloc);
    if (!out) { aether_caps_free(padded, padded_len); return 0; }

    int written = EVP_DecodeBlock(out, padded, (int)padded_len);
    aether_caps_free(padded, padded_len);
    if (written < 0) { aether_caps_free(out, out_alloc); return 0; }

    /* Trim trailing zero bytes that correspond to the padding we
     * added. EVP_DecodeBlock decodes '=' as 0, so the original
     * input's trailing-pad count tells us how many bytes to drop. */
    int pad_added = (int)(padded_len - in_len);
    int extra_pad_in_input = 0;
    while (extra_pad_in_input < (int)in_len &&
           in[in_len - 1 - extra_pad_in_input] == '=') {
        extra_pad_in_input++;
    }
    int trim = pad_added + extra_pad_in_input;
    if (trim > written) trim = written;
    written -= trim;

    g_b64_buf = out;
    g_b64_cap = out_alloc;
    g_b64_len = written;
    return 1;
}

const char* cryptography_get_base64_decode(void) {
    return g_b64_buf ? (const char*)g_b64_buf : "";
}

int cryptography_get_base64_decode_length(void) {
    return g_b64_len;
}

#else /* !AETHER_HAS_OPENSSL */

char* cryptography_sha1_hex_raw(const char* data, int length) {
    (void)data; (void)length; return NULL;
}
char* cryptography_sha256_hex_raw(const char* data, int length) {
    (void)data; (void)length; return NULL;
}
char* cryptography_hash_hex_raw(const char* algo, const char* data, int length) {
    (void)algo; (void)data; (void)length; return NULL;
}
int cryptography_hash_supported(const char* algo) {
    (void)algo; return 0;
}
char* cryptography_base64_encode_raw(const char* data, int length) {
    (void)data; (void)length; return NULL;
}
char* cryptography_base64_encode_padded_raw(const char* data, int length) {
    (void)data; (void)length; return NULL;
}
int cryptography_base64_decode_raw(const char* b64) {
    (void)b64; return 0;
}
const char* cryptography_get_base64_decode(void) { return ""; }
int cryptography_get_base64_decode_length(void) { return 0; }
void cryptography_release_base64_decode(void) {}

#endif /* AETHER_HAS_OPENSSL */
