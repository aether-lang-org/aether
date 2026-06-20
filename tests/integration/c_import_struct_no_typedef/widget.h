/* A C header that owns a struct WITHOUT shipping a convenience
 * typedef.  This is the shape of POSIX's `struct tm` in <time.h>,
 * `struct stat` in <sys/stat.h>, `struct sockaddr` in <sys/socket.h>,
 * and several other system structs.  The Aether port needs to be able
 * to declare them with `@c_import` and have generated C compile —
 * which means aetherc must emit `struct widget *` rather than the
 * bare `widget *` that requires a typedef.
 *
 * Force-included into the Aether-generated TU via aether.toml's
 * `cflags = "-include widget.h"`. */
#ifndef C_IMPORT_STRUCT_NO_TYPEDEF_WIDGET_H
#define C_IMPORT_STRUCT_NO_TYPEDEF_WIDGET_H

/* Deliberately NO `typedef struct widget widget;` here. */
struct widget {
    int   serial;
    int   weight;
    void* payload;
};

#endif
