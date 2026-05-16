// spawn_sandboxed_bsd.c — spawn a child process under Aether sandbox (FreeBSD)
//
// Same model as the Linux backend: fork, shm_open, and LD_PRELOAD —
// FreeBSD's rtld-elf honors LD_PRELOAD and dlsym(RTLD_NEXT, ...) just
// as glibc's loader does. The one platform difference is locating the
// running binary: FreeBSD does not mount /proc by default, so instead
// of readlink("/proc/self/exe") we ask the kernel directly via
// sysctl(KERN_PROC_PATHNAME).
//
// This is the LD_PRELOAD-parity backend only. OS-enforced containment
// via Capsicum (cap_enter / cap_rights_limit) is separate, future work
// — see docs/aether_compared_to_capsicum.md.
//
// Self-guarded: compiles to an empty object off FreeBSD. Companion
// backends: spawn_sandboxed_linux.c, spawn_sandboxed_stub.c.

#if defined(__FreeBSD__)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <fcntl.h>

// From libaether.a — list operations
extern int list_size(void*);
extern void* list_get_raw(void*, int);

// Find libaether_sandbox.so next to the running binary.
// FreeBSD has no /proc/self/exe by default; KERN_PROC_PATHNAME is the
// kernel-supported way to get the executable's absolute path.
static int find_preload_path(char* buf, int bufsize) {
    // exe[] holds the binary path; kept well under bufsize so the
    // snprintf below (path + "build/libaether_sandbox.so") cannot
    // truncate. Matches the sizing in spawn_sandboxed_linux.c.
    char exe[512];
    size_t exe_len = sizeof(exe);
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };

    if (sysctl(mib, 4, exe, &exe_len, NULL, 0) != 0 || exe_len == 0) {
        // Fallback: look in current directory
        snprintf(buf, bufsize, "./libaether_sandbox.so");
        return access(buf, F_OK) == 0;
    }
    // sysctl reports exe_len including the trailing NUL; guard anyway.
    if (exe_len < sizeof(exe)) exe[exe_len] = '\0';
    else exe[sizeof(exe) - 1] = '\0';

    // Strip binary name, keep directory
    char* slash = strrchr(exe, '/');
    if (slash) *(slash + 1) = '\0';

    snprintf(buf, bufsize, "%slibaether_sandbox.so", exe);
    if (access(buf, F_OK) == 0) return 1;

    // Try ../build/
    if (slash) *slash = '\0';
    slash = strrchr(exe, '/');
    if (slash) *(slash + 1) = '\0';
    snprintf(buf, bufsize, "%sbuild/libaether_sandbox.so", exe);
    if (access(buf, F_OK) == 0) return 1;

    return 0;
}

// Serialize grants from a list to shared memory
// Format: "category:pattern\n" lines, null-terminated
static char shm_name[64];

static int serialize_grants(void* grant_list) {
    int n = list_size(grant_list);
    if (n <= 0 || n % 2 != 0) return -1;

    // Build the grant string
    char buf[8192];
    int pos = 0;
    for (int i = 0; i < n && pos < 8000; i += 2) {
        const char* cat = (const char*)list_get_raw(grant_list, i);
        const char* pat = (const char*)list_get_raw(grant_list, i + 1);
        if (!cat || !pat) continue;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s:%s\n", cat, pat);
    }

    // Write to shared memory
    snprintf(shm_name, sizeof(shm_name), "/aether_sandbox_%d", getpid());
    int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return -1;

    if (ftruncate(fd, pos + 1) != 0) {
        close(fd);
        shm_unlink(shm_name);
        return -1;
    }
    void* mem = mmap(NULL, pos + 1, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) { close(fd); return -1; }

    memcpy(mem, buf, pos + 1);
    munmap(mem, pos + 1);
    close(fd);

    return 0;
}

// Spawn a sandboxed child process
// Returns: child exit code, or -1 on error
int aether_spawn_sandboxed(void* grant_list, const char* program, const char* arg) {
    // Find the preload library
    char preload_path[1024];
    if (!find_preload_path(preload_path, sizeof(preload_path))) {
        fprintf(stderr, "[aether] cannot find libaether_sandbox.so\n");
        return -1;
    }

    // Serialize grants to shared memory
    if (serialize_grants(grant_list) < 0) {
        fprintf(stderr, "[aether] cannot create shared memory for grants\n");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        shm_unlink(shm_name);
        return -1;
    }

    if (pid == 0) {
        // Child: set up LD_PRELOAD and grant source, then exec
        setenv("LD_PRELOAD", preload_path, 1);
        setenv("AETHER_SANDBOX_SHM", shm_name, 1);
        setenv("AETHER_SANDBOX_VERBOSE", "0", 0);

        if (arg) {
            execlp(program, program, arg, NULL);
        } else {
            execlp(program, program, NULL);
        }
        perror("exec");
        _exit(127);
    }

    // Parent: wait for child
    int status = 0;
    waitpid(pid, &status, 0);

    // Cleanup shared memory
    shm_unlink(shm_name);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

#endif // __FreeBSD__
