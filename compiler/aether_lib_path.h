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

#endif /* AETHER_LIB_PATH_H */
