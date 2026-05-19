#ifndef AETHER_RUNTIME_H
#define AETHER_RUNTIME_H

#include "utils/aether_thread.h"
#include <stdint.h>

// Error codes
#define AETHER_SUCCESS 0
#define AETHER_ERROR_OUT_OF_MEMORY -1
#define AETHER_ERROR_INVALID_ARGUMENT -2
#define AETHER_ERROR_NOT_INITIALIZED -3

// Runtime configuration flags
#define AETHER_FLAG_AUTO_DETECT      (1 << 0)  // Auto-detect CPU features (recommended)
#define AETHER_FLAG_LOCKFREE_MAILBOX (1 << 1)  // Use lock-free SPSC mailboxes
#define AETHER_FLAG_LOCKFREE_POOLS   (1 << 2)  // Use lock-free TLS message pools
#define AETHER_FLAG_ENABLE_SIMD      (1 << 3)  // Enable AVX2 vectorization
#define AETHER_FLAG_ENABLE_MWAIT     (1 << 4)  // Enable MWAIT for idle
#define AETHER_FLAG_VERBOSE          (1 << 5)  // Print configuration on init

// Runtime configuration structure
typedef struct {
    int num_cores;
    int flags;
    int use_lockfree_mailbox;
    int use_lockfree_pools;
    int use_mwait;
    int use_simd;
} AetherRuntimeInitConfig;

// Runtime initialization
void aether_runtime_init(int num_cores, int flags);
void aether_runtime_shutdown();

// Command-line arguments (set by main, accessible from anywhere)
extern int aether_argc;
extern char** aether_argv;
void aether_args_init(int argc, char** argv);
int aether_args_count(void);
const char* aether_args_get(int index);

// Return argv[0] — the path the OS launched the current process with.
// Borrowed pointer into the OS argv storage; do not free. Returns NULL
// when aether_args_init has not been called.
const char* aether_argv0(void);

// Return the raw argv vector captured by aether_args_init. Borrowed pointer
// into the OS argv storage; do not free or retain beyond process lifetime.
char** aether_argv_raw(void);

// Configuration queries
const AetherRuntimeInitConfig* aether_runtime_get_config();
int aether_runtime_has_feature(int feature_flag);
void aether_runtime_print_config();

// Aether built-in `sleep(ms)` lowers to a call to this — a stable,
// prefixed symbol that won't collide with libc's `sleep` if user code
// declares `extern sleep(...)` for some other purpose. See issue #233.
void aether_sleep_ms(int ms);

// Legacy compatibility
static inline void aether_init() {
    aether_runtime_init(0, AETHER_FLAG_AUTO_DETECT);
}

static inline void aether_cleanup() {
    aether_runtime_shutdown();
}

#endif
