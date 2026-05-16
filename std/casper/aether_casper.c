// aether_casper.c — FreeBSD Casper service bindings for std.casper.
// See aether_casper.h for the contract and the two-phase usage model.
//
// VENDORED DECLARATIONS: this GhostBSD / FreeBSD host carries the Casper
// runtime libraries (libcasper.so.1, libcap_net, libcap_pwd,
// libcap_sysctl) but not their development headers (<libcasper.h>,
// <casper/cap_*.h>). Rather than require a base-source checkout, the
// handful of Casper prototypes std.casper needs are declared inline
// below, guarded to FreeBSD. The structs those functions use
// (struct addrinfo, struct passwd) come from the standard <netdb.h> /
// <pwd.h>, which ARE present — only the Casper entry points are
// vendored. If a future FreeBSD changes these signatures this block is
// the single place to update; the runtime .so is what actually
// resolves the symbols at load time.

#include "aether_casper.h"

#if defined(__FreeBSD__)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>        // struct addrinfo, AI_* — standard, present
#include <pwd.h>          // struct passwd — standard, present
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>    // inet_ntop

// --- Vendored Casper prototypes (normally <libcasper.h>) ---
typedef struct cap_channel cap_channel_t;
extern cap_channel_t* cap_init(void);
extern cap_channel_t* cap_service_open(const cap_channel_t* chan,
                                       const char* name);
extern void cap_close(cap_channel_t* chan);

// --- Vendored cap_net prototype (normally <casper/cap_net.h>) ---
extern int cap_getaddrinfo(cap_channel_t* chan, const char* hostname,
                           const char* servname,
                           const struct addrinfo* hints,
                           struct addrinfo** res);

// --- Vendored cap_pwd prototype (normally <casper/cap_pwd.h>) ---
extern struct passwd* cap_getpwnam(cap_channel_t* chan, const char* name);

// --- Vendored cap_sysctl prototype (normally <casper/cap_sysctl.h>) ---
extern int cap_sysctlbyname(cap_channel_t* chan, const char* name,
                            void* oldp, size_t* oldlenp,
                            const void* newp, size_t newlen);

int aether_casper_available(void) {
    // The only honest probe is to actually reach the daemon: cap_init()
    // succeeds when casperd is available. Close the trial channel
    // immediately — callers open their own.
    cap_channel_t* c = cap_init();
    if (!c) return 0;
    cap_close(c);
    return 1;
}

void* aether_casper_init(void) {
    return (void*)cap_init();
}

void* aether_casper_service_open(void* casper, const char* service) {
    if (!casper || !service) return NULL;
    return (void*)cap_service_open((cap_channel_t*)casper, service);
}

void aether_casper_close(void* chan) {
    if (chan) cap_close((cap_channel_t*)chan);
}

char* aether_casper_resolve(void* netchan, const char* hostname) {
    if (!netchan || !hostname) return NULL;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;       // IPv4 or IPv6, whichever resolves
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = NULL;
    int rc = cap_getaddrinfo((cap_channel_t*)netchan, hostname, NULL,
                             &hints, &res);
    if (rc != 0 || !res) return NULL;

    // Stringify the first result's address into a fresh heap buffer.
    char buf[INET6_ADDRSTRLEN];
    buf[0] = '\0';
    if (res->ai_family == AF_INET) {
        struct sockaddr_in* sa = (struct sockaddr_in*)res->ai_addr;
        inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
    } else if (res->ai_family == AF_INET6) {
        struct sockaddr_in6* sa6 = (struct sockaddr_in6*)res->ai_addr;
        inet_ntop(AF_INET6, &sa6->sin6_addr, buf, sizeof(buf));
    }
    freeaddrinfo(res);
    if (buf[0] == '\0') return NULL;
    return strdup(buf);
}

long aether_casper_pwd_uid(void* pwdchan, const char* username) {
    if (!pwdchan || !username) return -1;
    struct passwd* pw = cap_getpwnam((cap_channel_t*)pwdchan, username);
    if (!pw) return -1;
    return (long)pw->pw_uid;
}

char* aether_casper_pwd_home(void* pwdchan, const char* username) {
    if (!pwdchan || !username) return NULL;
    struct passwd* pw = cap_getpwnam((cap_channel_t*)pwdchan, username);
    if (!pw || !pw->pw_dir) return NULL;
    return strdup(pw->pw_dir);
}

char* aether_casper_sysctl_str(void* sysctlchan, const char* name) {
    if (!sysctlchan || !name) return NULL;
    char buf[1024];
    size_t len = sizeof(buf) - 1;
    int rc = cap_sysctlbyname((cap_channel_t*)sysctlchan, name,
                              buf, &len, NULL, 0);
    if (rc != 0) return NULL;
    // cap_sysctlbyname writes `len` bytes; NUL-terminate defensively in
    // case the value was not already terminated.
    if (len < sizeof(buf)) buf[len] = '\0';
    else buf[sizeof(buf) - 1] = '\0';
    return strdup(buf);
}

#else // not FreeBSD — Casper does not exist

#include <stddef.h>   // NULL

void* aether_casper_init(void) { return NULL; }
void* aether_casper_service_open(void* c, const char* s) {
    (void)c; (void)s; return NULL;
}
void aether_casper_close(void* c) { (void)c; }
int aether_casper_available(void) { return 0; }

char* aether_casper_resolve(void* n, const char* h) {
    (void)n; (void)h; return 0;
}
long aether_casper_pwd_uid(void* p, const char* u) {
    (void)p; (void)u; return -1;
}
char* aether_casper_pwd_home(void* p, const char* u) {
    (void)p; (void)u; return 0;
}
char* aether_casper_sysctl_str(void* s, const char* n) {
    (void)s; (void)n; return 0;
}

#endif // __FreeBSD__
