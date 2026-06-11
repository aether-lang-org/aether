/* tools/ae_compat.h — Windows/POSIX compatibility shims for the
 * `ae` CLI and its `ae help` companion (tools/ae.c, tools/ae_help.c).
 *
 * Currently provides one shim, but the file exists so future shims
 * (path normalisation, time, env access) land in the same place
 * instead of being scattered through both translation units.
 *
 * stat()/struct stat:
 *   WinLibs UCRT's <sys/stat.h> redirects POSIX stat() to the asm
 *   symbol stat64i32 via __MINGW_ASM_CALL. No shipped static library
 *   provides that symbol, so `gcc -static tools/ae.c tools/ae_help.c …`
 *   fails at link with `undefined reference to stat64i32`. The aether
 *   runtime already sidesteps this by calling _stat64 directly on
 *   _WIN32; this header exports the same dodge to the CLI tools.
 *
 *   Field names (st_mode / st_size / st_mtime) line up across the two
 *   backends. st_mtime widens to __time64_t on Windows; every use
 *   site already casts to (long long), which absorbs the widening.
 *
 *   Usage:  ae_stat_t st;
 *           if (ae_stat(path, &st) == 0 && S_ISREG(st.st_mode)) { ... }
 */

#ifndef AE_COMPAT_H
#define AE_COMPAT_H

#include <sys/stat.h>

#ifdef _WIN32
typedef struct _stat64 ae_stat_t;
#define ae_stat(p, st) _stat64((p), (st))
#else
typedef struct stat ae_stat_t;
#define ae_stat(p, st) stat((p), (st))
#endif

#endif /* AE_COMPAT_H */
