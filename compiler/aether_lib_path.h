/* aether_lib_path.h — single source of truth for the lib-search-path
 * separator and capacity. Included by both the compiler-side parser
 * (`module_set_lib_dir` in compiler/aether_module.c) and the CLI-side
 * appender (`tc_lib_dir_append` in tools/ae.c) so the two sides of
 * the toolchain agree on what `:` (POSIX) / `;` (Windows) means in
 * `--lib`, `AETHER_LIB_DIR`, and any future plumbing.
 *
 * Standalone header so `tools/ae.c` doesn't need to pull
 * `compiler/aether_module.h` (and transitively `compiler/ast.h`)
 * just to get a single character constant. Issue #413. */

#ifndef AETHER_LIB_PATH_H
#define AETHER_LIB_PATH_H

#include <stddef.h>  /* size_t for the path-normalisation helper */

/* Soft cap on the number of entries in the lib-search path.
 * PATH-style chains rarely exceed 4 in practice; 8 covers any
 * realistic project layering. Excess entries are dropped with a
 * one-line stderr warning rather than aborting the toolchain, so a
 * misconfigured shell `AETHER_LIB_DIR` can never crash a build. */
#define AETHER_LIB_DIRS_MAX 8

/* PATH-style separator. `:` on POSIX (matches Java -cp, Python
 * PYTHONPATH, Ruby RUBYLIB), `;` on Windows (where `:` is part of
 * every drive-letter path). */
#ifdef _WIN32
#define AETHER_LIB_PATH_SEP_CHAR ';'
#else
#define AETHER_LIB_PATH_SEP_CHAR ':'
#endif

/* Normalise an MSYS2/Cygwin POSIX-style path to native Windows form.
 *
 * The motivation (Windows-only): MSYS2 bash's $(pwd) yields
 * `/d/a/aether/...` style paths. When passed as a *single* argv to a
 * native Win32 binary, MSYS2's argv-translator converts that to
 * `D:\a\aether\...`. But when passed as a `;`-joined PATH-style
 * LIST (`/d/a/.../dirA;/d/a/.../dirB`) the translator does NOT
 * recognise the list shape and leaves the POSIX-style paths
 * untouched. ae splits the list correctly but the per-entry paths
 * are then in `/d/...` form — which Windows `fopen()` reliably
 * fails to open. The repeated-`--lib` form (one path per argv) is
 * fine because the translator handles each arg individually.
 *
 * Fix: normalise `/x/...` → `x:/...` at the entry point on both
 * sides of the toolchain (`module_add_lib_dir` on the compiler side,
 * `tc_lib_dir_append_one` on the CLI side), so a path-list and a
 * sequence of flags end up byte-identical regardless of how MSYS2
 * handled the argv. On POSIX this function is a pure copy.
 *
 * Output buffer must hold up to `strlen(in)` bytes plus a NUL (no
 * expansion possible since `/x/` → `x:/` is the same width). The
 * function is small enough to inline. Issue #413 Windows follow-up. */
static inline void aether_lib_path_normalize(const char* in, char* out, size_t out_size) {
    if (out_size == 0) return;
    if (!in) { out[0] = '\0'; return; }
#ifdef _WIN32
    /* Match the MSYS2 POSIX-drive form: `/<single-letter>/` at the
     * very start. Examples that match: `/d/foo`, `/c/`. Examples
     * that don't (and shouldn't be touched): `D:\foo`, `D:/foo`,
     * `./relative`, `lib`, `/usr/local/share` (no drive-letter
     * convention). */
    if (in[0] == '/' && in[1] && in[2] == '/' &&
        ((in[1] >= 'a' && in[1] <= 'z') || (in[1] >= 'A' && in[1] <= 'Z'))) {
        char drive_upper = (char)(in[1] >= 'a' ? in[1] - ('a' - 'A') : in[1]);
        if (out_size >= 3) {
            out[0] = drive_upper;
            out[1] = ':';
            out[2] = '/';
            /* Copy the rest verbatim, with forward slashes preserved
             * (Windows fopen accepts both `/` and `\`; staying with
             * `/` avoids any escape-character confusion). */
            size_t i = 3, j = 3;
            while (in[j] && i + 1 < out_size) {
                out[i++] = in[j++];
            }
            out[i] = '\0';
            return;
        }
    }
#endif
    /* Default: byte-copy, bounded. */
    size_t n = 0;
    while (in[n] && n + 1 < out_size) {
        out[n] = in[n];
        n++;
    }
    out[n] = '\0';
}

#endif /* AETHER_LIB_PATH_H */
