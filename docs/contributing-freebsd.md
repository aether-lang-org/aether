# FreeBSD + Capsicum Support

Aether is fully supported on FreeBSD 10.0 and later, with optional integration with **Capsicum**, a capability-based security framework built into the FreeBSD kernel.

## Building on FreeBSD

### Prerequisites

```bash
pkg update
pkg install gcc clang make pkgconf
```

### Compilation

Standard build commands work on FreeBSD:

```bash
make compiler ae stdlib
make test-ae
```

Aether auto-detects FreeBSD at compile time and enables Capsicum support when available.

### Capsicum Detection

The build system checks `uname -s` and sets `-DAETHER_HAS_CAPSICUM` on FreeBSD. If you need to force-disable Capsicum:

```bash
CAPSICUM=0 make compiler ae stdlib
```

## Using Capsicum in Aether Code

Capsicum is optional — Aether programs run without it, but you can opt-in to capability-based sandboxing:

```aether
import capsicum

main(args: [string]) {
    if capsicum.capsicum_available() {
        println("Capsicum available on this system")

        // Restrict access to a file descriptor to read-only
        fd = std.fs.open("data.txt", "r")
        capsicum.capsicum_limit_rights(fd, 0x0000000000000001)  // CAP_READ

        // Enter capability mode — irreversible, kernel-enforced
        if capsicum.capsicum_enter() == 0 {
            println("Entered capability mode")
            // From here on, only operations with restricted rights are allowed
            content = std.fs.read(fd)
            println("Read: ${content}")
        }
    } else {
        println("Capsicum not available")
    }
}
```

## Capsicum Functions

### `capsicum_available() -> int`

Returns 1 if Capsicum is available (FreeBSD 10.0+, kernel supports it), 0 otherwise.

**Example:**
```aether
if capsicum.capsicum_available() {
    // use Capsicum APIs
}
```

### `capsicum_enter() -> int`

Enters capability mode irreversibly. All subsequent operations are restricted to the capabilities you've granted to file descriptors.

Returns 0 on success, -1 on error.

**Important:** Once you call `capsicum_enter()`, you cannot exit capability mode or change the capabilities of fds. This is by design — Capsicum provides an ironclad guarantee that the process remains sandboxed.

**Example:**
```aether
result = capsicum.capsicum_enter()
if result == 0 {
    println("Successfully entered capability mode")
} else {
    println("Failed to enter capability mode")
}
```

### `capsicum_limit_rights(fd: int, rights: u64) -> int`

Restrict a file descriptor to specific capabilities. Must be called before `capsicum_enter()`.

Returns 0 on success, -1 on error.

**Capability Rights (common flags):**
- `0x0000000000000001` — `CAP_READ` — read
- `0x0000000000000002` — `CAP_WRITE` — write
- `0x0000000000000004` — `CAP_SEEK` — seek
- `0x0000000000000008` — `CAP_PREAD` — pread
- `0x0000000000000010` — `CAP_PWRITE` — pwrite

For a complete list, see `<sys/capsicum.h>` on FreeBSD or the [Capsicum man pages](https://www.freebsd.org/cgi/man.cgi?capsicum).

**Example:**
```aether
fd = std.fs.open("data.txt", "r")
// Restrict to read-only
capsicum.capsicum_limit_rights(fd, 0x0000000000000001)  // CAP_READ
capsicum.capsicum_enter()
```

### `capsicum_pdwait4(pd: int, status_ptr: ptr, options: int, rusage_ptr: ptr) -> int`

Wait on a sandboxed child process descriptor. Used in advanced scenarios where you've spawned a child and need to wait for it within capability mode.

Returns the PID of the child on success, -1 on error.

**Example:**
```aether
// Spawn a child process
child_pid = std.os.fork()
if child_pid == 0 {
    // Child side
    capsicum.capsicum_enter()
    // child work...
    std.os.exit(0)
} else {
    // Parent side
    capsicum.capsicum_enter()
    // Wait for child
    status = 0
    capsicum.capsicum_pdwait4(child_pid, &status, 0, null)
}
```

## Capsicum vs. Aether's Language-Level Sandbox

Aether provides **three layers** of sandboxing:

1. **Language-level** (closure scoping + permission contexts) — portable, all platforms
2. **Host-module** (integration with Ruby, Python, Go, Java, etc.) — enforced by permission checker
3. **LD_PRELOAD** (syscall interception) — Linux/macOS/FreeBSD, userspace enforcement
4. **Capsicum** (optional, FreeBSD only) — kernel-enforced, ironclad, irreversible

Capsicum is the strongest layer because it's kernel-enforced and cannot be bypassed from userspace. However, it's FreeBSD-only and requires careful design (capability mode is irreversible).

**Best practice:** Use Capsicum as the outermost layer to guarantee OS-level isolation when running on FreeBSD.

## Limitations

### Not Available on Linux Containers

If you're running a FreeBSD OCI container on a Linux host, Capsicum syscalls will fail (require FreeBSD kernel). Aether detects this gracefully — `capsicum_available()` returns 0, and code can fall back to other sandboxing layers.

### Capability Mode is Irreversible

Once you call `capsicum_enter()`, you cannot exit or change fd restrictions. Design your program to restrict fds **before** entering capability mode.

### Limited Casper Daemon Support

Capsicum's Casper daemon (for privileged operations like DNS, getpwd, etc. delegation) is not yet integrated into Aether. To use Casper services, you'll need to call the syscalls directly via FFI or wait for Phase 2 work (see [GitHub issue #402](https://github.com/aether-lang-org/aether/issues/402)).

## Testing on FreeBSD

### Local FreeBSD VM

To test locally, set up a FreeBSD VM (e.g., via VirtualBox, Vagrant, or cloud provider):

```bash
# On FreeBSD host
git clone <aether-repo>
cd aether
make ci  # Full CI suite
```

### Cirrus CI

Aether CI includes a native FreeBSD 13.3 job (via Cirrus CI). Every PR is tested on FreeBSD; compilation and integration tests must pass.

## Resources

- [Capsicum Research Paper](https://www.cl.cam.ac.uk/research/security/capsicum/)
- [FreeBSD Capsicum Man Pages](https://www.freebsd.org/cgi/man.cgi?capsicum)
- [FreeBSD Security Documentation](https://docs.freebsd.org/en/books/security/)
- [Aether vs. BSD Capsicum Comparison](./aether_compared_to_capsicum.md)

## Future Work

See [GitHub issue #402](https://github.com/aether-lang-org/aether/issues/402) for:
- **Phase 2:** Transparent runtime integration (auto-use Capsicum for actor sandboxing)
- **Phase 3:** Capability audit logging for forensics
- **Phase 4:** Casper daemon and jail integration
