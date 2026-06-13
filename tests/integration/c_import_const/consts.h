/* C header that owns the object-like macros the Aether probe imports
 * via `extern const NAME: type @c_import` (#702). Stands in for the
 * platform headers (<errno.h>, <fcntl.h>, release.h) whose macros the
 * Redis port needs to reference by name rather than inline by value.
 *
 * Force-included into every translation unit (including the Aether
 * .gen.c) via `cflags = "-include consts.h"` in aether.toml. The
 * generated C emits these names verbatim at use sites and defines
 * nothing itself, so the header is the sole source of truth. */
#ifndef C_IMPORT_CONST_H
#define C_IMPORT_CONST_H

#define MY_FLAG   7              /* object-like int macro            */
#define MY_LIMIT  1000           /* used in a comparison context     */
#define MY_BUILD  "aether-702"   /* string macro -> const char *     */

#endif
