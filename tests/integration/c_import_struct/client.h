/* The C header that owns the `client` struct.  Stands in for a Redis
 * header (`server.h`) that already defines `client`, `robj`, `dict`,
 * etc.  The Aether side declares `client` with `@c_import` so it
 * typechecks field access against this layout WITHOUT emitting a
 * competing `typedef struct client { ... } client;` of its own.
 *
 * Force-included into every translation unit (including the Aether
 * .gen.c) via `cflags = "-include client.h"` in aether.toml. */
#ifndef C_IMPORT_STRUCT_CLIENT_H
#define C_IMPORT_STRUCT_CLIENT_H

typedef struct client {
    int   argc;
    void *argv;
    int   fd;
} client;

#endif
