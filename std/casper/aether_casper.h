#ifndef AETHER_CASPER_H
#define AETHER_CASPER_H

// std.casper — FreeBSD Casper service delegation.
//
// A process in Capsicum capability mode (see std.capsicum) can no
// longer reach global namespaces: no DNS, no /etc/passwd, no sysctl.
// Casper bridges that — a separate, unsandboxed casper daemon performs
// those operations on the sandboxed process's behalf, over a channel
// opened *before* cap_enter().
//
// The model is strictly two-phase:
//   1. While still unconfined: cap_init() to reach the casper daemon,
//      then cap_service_open() for each service you'll need.
//   2. cap_enter() (via std.capsicum).
//   3. Inside capability mode: the service channels still work — DNS
//      resolution, passwd lookups, sysctl reads succeed via delegation.
//
// A channel opened after cap_enter() fails; this binding cannot fix
// that — it is a property of how Casper works.
//
// This binding flattens the service results to Aether-friendly scalars
// and strings (struct addrinfo / struct passwd never cross the
// boundary). Off FreeBSD every entry point is a stub returning failure,
// so code importing std.casper builds and links everywhere.
//
// Channels are passed to Aether as opaque `void*` (cap_channel_t*).

// ---- Channel lifecycle ----

// Connect to the casper daemon. Returns an opaque channel handle, or
// NULL on failure / non-FreeBSD. Must be called BEFORE cap_enter().
void* aether_casper_init(void);

// Open a named service channel from the casper handle returned by
// aether_casper_init(). `service` is e.g. "system.net", "system.pwd",
// "system.sysctl". Returns an opaque service-channel handle or NULL.
// Must be called BEFORE cap_enter().
void* aether_casper_service_open(void* casper, const char* service);

// Close a channel (casper handle or service handle). Safe on NULL.
void aether_casper_close(void* chan);

// 1 if Casper is usable (FreeBSD with the casper daemon), else 0.
int aether_casper_available(void);

// The three lookup functions below return a heap-allocated string the
// caller owns and must free, or NULL on failure. Returning by heap
// (rather than a caller-supplied buffer) is the idiomatic Aether-extern
// shape — matches os_getenv / os_which.

// ---- DNS service (system.net) ----

// Resolve `hostname` to its first IP address via the DNS service
// channel `netchan`. Returns a heap string with the printable address
// (IPv4 or IPv6), or NULL on failure. Works inside capability mode.
char* aether_casper_resolve(void* netchan, const char* hostname);

// ---- Password service (system.pwd) ----

// Look up `username` via the pwd service channel. Returns the numeric
// uid, or -1 if the user is not found / on error.
long aether_casper_pwd_uid(void* pwdchan, const char* username);

// Look up `username`'s home directory via the pwd service channel.
// Returns a heap string, or NULL on failure.
char* aether_casper_pwd_home(void* pwdchan, const char* username);

// ---- sysctl service (system.sysctl) ----

// Read the sysctl named `name` as a string via the sysctl service
// channel. Returns a heap string, or NULL on failure. Meant for
// string-valued sysctls (kern.hostname, kern.ostype, ...); numeric
// sysctls come back as raw bytes and are not useful here.
char* aether_casper_sysctl_str(void* sysctlchan, const char* name);

#endif // AETHER_CASPER_H
