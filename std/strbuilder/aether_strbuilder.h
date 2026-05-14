#ifndef AETHER_STRBUILDER_H
#define AETHER_STRBUILDER_H

#include <stddef.h>

/* std.strbuilder — amortised-O(1) string append primitive.
 *
 * (Module is named `strbuilder` rather than `builder` because the
 * latter is reserved in Aether for configure-then-execute DSL
 * functions. The API mirrors Java/Kotlin StringBuilder, Go's
 * strings.Builder, Rust's String::push_str.)
 *
 * The canonical accumulator idiom `out = string.concat(out, chunk)`
 * in a loop is O(N²) per byte because every concat reallocates and
 * memcpy's the full prefix. A correctly-tracked heap-string just
 * means each leak is freed promptly; the algorithmic cost stays.
 *
 * AetherStrBuilder solves that with a growing internal buffer (2×
 * doubling on overflow, amortised-O(1) per byte appended). The
 * lifecycle is:
 *
 *     b = strbuilder.new(cap_hint)
 *     strbuilder.append(b, chunk)     // any number of times
 *     out = strbuilder.finish(b)      // hand the buffer off; b invalid
 *
 * strbuilder.finish hands the accumulated bytes to a refcounted
 * AetherString and destroys the builder. The returned string is
 * heap-owned (caller frees / heap-tracker reclaims at scope exit).
 * If a caller decides mid-build to abandon the work,
 * strbuilder.free(b) discards without producing a string.
 *
 * The append surface mirrors what's needed by the avn-style
 * dir-blob, JSON / log / template / packed-record building patterns
 * — bytes, full strings, int decimals, and explicit-length strings
 * for binary-safe content (embedded NULs round-trip). */

typedef struct AetherStrBuilder AetherStrBuilder;

/* Allocate a new builder with at least `cap_hint` bytes reserved. If
 * `cap_hint` is <= 0, a small default (64) is used. Returns NULL on
 * OOM / cap exceeded. Initial logical length is 0. */
AetherStrBuilder* aether_strbuilder_new(int cap_hint);

/* Append a string. Length is taken from the AetherString header when
 * `s` is a refcounted string, else via strlen. Returns 1 on success,
 * 0 on failure (NULL builder, NULL s, OOM, or cap exceeded). */
int aether_strbuilder_append(AetherStrBuilder* b, const void* s);

/* Append exactly `n` bytes from `s`. Binary-safe — embedded NULs
 * round-trip. `s` may be AetherString* (read via aether_string_data)
 * or plain `const char*`. Returns 1 on success, 0 on failure
 * (negative n, NULL s, OOM, etc.). n == 0 is a no-op success. */
int aether_strbuilder_append_n(AetherStrBuilder* b, const void* s, int n);

/* Append a single byte (low 8 bits of `c`). Useful for separators
 * ('\n', ',', 0x02, ...). Returns 1 on success, 0 on failure. */
int aether_strbuilder_append_byte(AetherStrBuilder* b, int c);

/* Append the decimal representation of `v` (no padding, with a
 * leading '-' for negatives). Avoids the round-trip through
 * string.from_int + strbuilder.append. Returns 1 on success, 0 on
 * failure. */
int aether_strbuilder_append_int(AetherStrBuilder* b, int v);

/* Current accumulated length in bytes, or -1 if `b` is NULL. */
int aether_strbuilder_length(AetherStrBuilder* b);

/* Finalise into a refcounted AetherString and destroy the builder.
 * After this call, `b` is invalid (do NOT call any strbuilder.* on
 * it again — not even free). Returns NULL on NULL input or string
 * allocation failure; the builder is still destroyed in the latter
 * case so resources don't leak on an OOM finalise. */
void* aether_strbuilder_finish(AetherStrBuilder* b);

/* Discard the builder without producing a string. Idempotent on
 * NULL. Use this on error paths where the caller has decided
 * partway through that the in-progress output isn't needed. */
void aether_strbuilder_free(AetherStrBuilder* b);

#endif  /* AETHER_STRBUILDER_H */
