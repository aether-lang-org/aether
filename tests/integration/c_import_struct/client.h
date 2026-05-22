/* The C header that owns the `client` struct. Stands in for POSIX headers
 * such as <time.h> that expose `struct tm` without a matching `tm` typedef.
 * The Aether side declares `client` with `@c_import`, so generated C must
 * refer to pointers as `struct client*`, not bare `client*`.
 *
 * Force-included into every translation unit (including the Aether
 * .gen.c) via `cflags = "-include client.h"` in aether.toml. */
#ifndef C_IMPORT_STRUCT_CLIENT_H
#define C_IMPORT_STRUCT_CLIENT_H

struct client {
    int   argc;
    void *argv;
    int   fd;
};

#endif
