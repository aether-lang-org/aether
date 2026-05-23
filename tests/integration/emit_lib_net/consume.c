/*
 * consume.c — C host for the --emit=lib --with=net PIC round-trip test.
 *
 * dlopens the shared library built from a std.http-importing module
 * (so the link pulled in the TLS-using HTTP runtime object) and calls
 * its aether_* exports. Proves the .so both links and loads — the thing
 * that was impossible before the runtime archive was built -fPIC.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

typedef int32_t (*aether_ping_fn)(void);
typedef const char* (*aether_echo_fn)(const char*);

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-libnetmod.so>\n", argv[0]);
        return 2;
    }

    void* h = dlopen(argv[1], RTLD_NOW);
    if (!h) {
        fprintf(stderr, "FAIL: dlopen(%s): %s\n", argv[1], dlerror());
        return 1;
    }

    aether_ping_fn ping = (aether_ping_fn)dlsym(h, "aether_ping");
    aether_echo_fn echo = (aether_echo_fn)dlsym(h, "aether_echo");
    if (!ping) { fprintf(stderr, "FAIL: aether_ping not found: %s\n", dlerror()); return 1; }
    if (!echo) { fprintf(stderr, "FAIL: aether_echo not found: %s\n", dlerror()); return 1; }

    int32_t p = ping();
    if (p != 7) {
        fprintf(stderr, "FAIL: aether_ping() = %d, expected 7\n", p);
        return 1;
    }

    const char* e = echo("hi");
    if (!e || strcmp(e, "echo:hi") != 0) {
        fprintf(stderr, "FAIL: aether_echo(\"hi\") = %s, expected \"echo:hi\"\n", e ? e : "(null)");
        return 1;
    }

    dlclose(h);
    printf("OK: --emit=lib --with=net .so links, dlopens, and round-trips\n");
    return 0;
}
