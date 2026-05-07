# Aether Sandbox Model vs. BSD Capsicum: Architecture, Parallels, and FreeBSD Integration

## Executive Summary

Aether and BSD Capsicum are two fundamentally different approaches to capability-based sandboxing:

- **Capsicum** is an OS-level security framework (FreeBSD kernel + POSIX extensions) that restricts processes via file descriptor capabilities and capability mode.
- **Aether** is a language-level sandbox model (implemented in the compiler and runtime) that uses closures, permission contexts, and scope hygiene to restrict what code can access.

Despite the difference in scope (OS vs. language), Aether's closure-based isolation and permission hierarchy are **conceptually aligned with Capsicum's principles**. This document explores the parallels, gaps, and opportunities for Aether on FreeBSD—including the possibility of an Aether runtime that leverages Capsicum for OS-enforced containment of sandboxed actors.

---

## Part 1: Sandbox Models Compared

### 1.1 Capsicum: OS-Level Capability Restrictions

#### Design Principles

Capsicum extends the POSIX API with capability-based security primitives. The key insight: **treat file descriptors as capabilities** with fine-grained rights.

**Core concepts:**

| Concept | Meaning |
|---------|---------|
| **Capability** | A file descriptor with restricted rights (e.g., read-only, seek-only, ioctl-restricted) |
| **Capability mode** | A process state where global namespace access is denied; all I/O must derive from passed-in fds |
| **Capability rights** | Bitmask of allowed operations (CAP_READ, CAP_WRITE, CAP_SEEK, CAP_IOCTL, CAP_FCNTL, etc.) |
| **Process descriptor** | A handle to another process; can be used to query its state or revoke its capabilities |
| **Casper daemon** | A privileged service that sandboxed processes delegate to (e.g., for DNS, syslog) |

#### How Capsicum Works

1. **Start privileged** — A process begins with all capabilities (or a default set).
2. **Enter capability mode** — Call `cap_enter()`. This is **irreversible**; the process can never access global namespaces again.
3. **Restrict file descriptors** — Use `cap_rights_limit(fd, rights)` to reduce a fd's rights. This restriction is **hereditary**—child processes inherit the restricted fds.
4. **Delegate via fd-passing** — Pass restricted fds to other processes via IPC. They get only the rights you granted.
5. **Fail securely** — Attempts to access denied resources fail with ENOTCAPABLE (a new errno).

#### Example: tcpdump under Capsicum

```c
// Original tcpdump runs privileged, has access to everything
int main() {
    // ... capture packets, write to file ...
}

// Capsicum version: restrict early
int main() {
    // Open packet device and output file while privileged
    int pcap_fd = open_live(...);
    int out_fd = open("capture.pcap", O_WRONLY | O_CREAT);
    
    // Restrict both fds to their needed rights
    cap_rights_init(&rights, CAP_READ);
    cap_rights_limit(pcap_fd, &rights);
    
    cap_rights_init(&rights, CAP_WRITE);
    cap_rights_limit(out_fd, &rights);
    
    // Enter capability mode — can no longer open new files or access global state
    cap_enter();
    
    // Now run the packet capture loop with only pcap_fd and out_fd
    // ... capture packets, write to out_fd ...
    
    return 0;
}
```

**Key property:** Even if tcpdump is compromised, it cannot:
- Open new files (no access to `open()`)
- Bind to ports (no `socket()`)
- Execute commands (no `execve()`)
- Access network beyond the pcap fd

#### Capsicum in Practice

**Real-world Capsicum users:**
- tcpdump, dhclient, hastd, kdump, rwhod, ctld, iscsid (FreeBSD base system)
- Chromium sandboxing (with Google collaboration)
- Firefox (experimental)
- PostgreSQL (libpq sandboxing proposal)

**Why it's effective:**
1. OS-enforced — kernel checks rights on every system call
2. Fail-safe — denied operations return ENOTCAPABLE, not a silent failure
3. No performance overhead for allowed operations
4. Composable — can chain restricted fds arbitrarily deep
5. Casper daemon pattern — delegates privilege to a trusted service

---

### 1.2 Aether: Language-Level Scope-Based Isolation

#### Design Principles

Aether's sandbox model is actually **three-layered**, combining language-level, hosted-language, and OS-level enforcement:

**Layer 1: Language-level isolation** uses **three language features**:

1. **Closures** — hoisted functions that cannot reach parent locals unless explicitly passed
2. **Permission contexts** — a data structure (list/map) representing granted capabilities
3. **Scope hygiene** — `hide` and `seal except` declarations that prevent name lookup in outer scopes

**Layer 2: Hosted-language integration** — Host modules (Ruby, Python, Perl, Lua, Tcl, JavaScript, Go, Java) wrap foreign-language interpreters and:

- Install Aether's permission checker (`_aether_sandbox_checker`) before evaluating code
- Scrub runtime caches (e.g., Ruby's ENV hash) to remove unauthorized variables
- Push/pop the permission context on the Aether sandbox stack
- Intercept all libc calls from the host language (open, connect, execve, getenv)

**Layer 3: OS-level enforcement** — An LD_PRELOAD sandbox library (`libaether_sandbox_preload.so`) intercepts syscalls:

- Resolves paths (defeating symlink/`..` bypasses)
- Pattern-matches grants (category:resource)
- Blocks denied syscalls at the OS boundary
- Logs violations for forensics

This three-layer approach means:
- Aether code is protected by the compiler (layer 1)
- Host languages (Ruby, Python, Go) are protected by the host module (layer 2)
- Even if a host module is compromised, LD_PRELOAD blocks syscalls (layer 3)

**Core concepts:**

| Concept | Meaning |
|---------|---------|
| **Closure** | A function value that captures a subset of variables; hoisted away from its declaration scope (layer 1) |
| **Permission context** | A list/map of (category, resource_pattern) tuples representing what a scope can do (layer 1-3) |
| **Permission category** | A string like "tcp", "fs_read", "fs_write", "exec", "env" (layer 1-3) |
| **Resource pattern** | A glob or exact match for a resource (e.g., "db.internal:5432", "/tmp/*") (layer 1-3) |
| **Host module** | A C wrapper for a foreign language (Ruby, Python, Go) that installs the permission checker (layer 2) |
| **LD_PRELOAD sandbox** | A shared library that intercepts libc syscalls and blocks denied operations (layer 3) |
| **Hide** | A declaration that the enclosing scope cannot see names from outer scopes (layer 1) |
| **Seal except** | A declaration that the enclosing scope cannot see names except the ones listed (layer 1) |
| **Containment principle** | The container can see the contained, but the contained cannot reach the container (all layers) |

#### How Aether's Sandbox Works

1. **Create a permissions context** — Allocate a list to hold permission tuples.
2. **Grant permissions** — Call `grant_tcp(ctx, "host", port)`, `grant_fs_read(ctx, path)`, etc.
3. **Wrap code in a closure** — The sandboxed code is a closure that receives the context as its only input.
4. **Run contained code** — Call `call(closure, ctx)`. The closure cannot reach parent locals.
5. **Check permissions** — Sandboxed code calls `sandboxed_tcp_connect(ctx, host, port)` which checks the context before operating.
6. **Fail gracefully** — Permission check fails, returns 0 (or throws, depending on implementation).

#### Example: Aether Sandboxed Worker

```aether
import std.list

// Permission check
check_permission(perms: ptr, category: string, resource: string) {
    n = list.size(perms)
    for (i = 0; i < n; i += 2) {
        cat = list.get(perms, i)
        pat = list.get(perms, i + 1)
        if str_eq(cat, "*") == 1 && str_eq(pat, "*") == 1 { return 1 }
        if str_eq(cat, category) == 1 {
            if str_eq(pat, "*") == 1 { return 1 }
            if str_eq(pat, resource) == 1 { return 1 }
        }
    }
    return 0
}

// Sandboxed operation wrapper
sandboxed_tcp_connect(perms: ptr, host: string, port: int) {
    if check_permission(perms, "tcp", host) == 1 {
        // Real code: tcp.connect(host, port)
        return 1
    }
    return 0  // Denied
}

// Contained code as a closure
worker_code = |perms| {
    if sandboxed_tcp_connect(perms, "db.internal", 5432) == 1 {
        println("Connected to DB")
    } else {
        println("Connection denied")
    }
}

// Create and grant permissions
main() {
    db_worker = list.new()
    list.add(db_worker, "tcp")
    list.add(db_worker, "db.internal")
    
    // Run the closure with restricted context
    call(worker_code, db_worker)
    
    // worker_code cannot access globals, files, or anything else
    // It can only do what the permissions context allows
}
```

**Key properties:**

1. **Language-enforced** — The compiler ensures closures can't reach parent locals (layer 1).
2. **Type-safe** — Closure signatures are checked at compile time (layer 1).
3. **Host-language integration** — Foreign languages (Ruby, Python, Go) get the same isolation via host modules (layer 2).
4. **OS-level fallback** — LD_PRELOAD intercepts syscalls as a final enforcement layer (layer 3).
5. **Transparent** — Permission checks are just function calls; can be optimized for fully-trusted code.
6. **Composable** — Actors can nest, each with its own permission context.
7. **Portable** — Works on any OS; LD_PRELOAD is available on all POSIX systems.

#### Three-Layer Architecture Diagram

```
+======================== Aether Native Code =========================+
| Closure (can't reach parent locals) + Permission Context (layer 1) |
+======================================================================+
          |
          v
+========= Hosted Language Code (8 languages supported) ============+
| Ruby, Python, Perl, Lua, Tcl, JavaScript, Go, Java (layer 2)       |
| - In-process (Ruby, Python, Perl, Lua, Tcl, JS) or subprocess (Go, Java) |
| - Host module scrubs runtime caches (ENV, $LOAD_PATH, etc.)        |
| - Permission checker installed: all I/O checked before syscall     |
+====================================================================+
          |
          v
+===================== OS Boundary (libc) ===========================+
| LD_PRELOAD interception of open, connect, execve, getenv (layer 3)|
| - Resolves paths (defeats symlinks, .., etc.)                      |
| - Pattern-matches grants (category:resource)                       |
| - Blocks denied syscalls with errno = EACCES / EPERM              |
+======================================================================+
          |
          v
       [ Kernel ]
```

**Defense in depth:**
- If layer 1 is exploited: layer 2 host module still checks
- If layer 2 is exploited: layer 3 LD_PRELOAD still blocks
- If code is native Aether: layer 1 is sufficient; layers 2-3 are optional

---

### 1.3 Side-by-Side Comparison

| Aspect | Capsicum | Aether (Layer 1) | Aether (Layers 1-3) |
|--------|----------|------------------|-------------------|
| **Enforcement layer** | OS kernel (syscall-level) | Language runtime (function-call level) | Language + host module + LD_PRELOAD (syscall interception) |
| **Isolation mechanism** | File descriptor restrictions + capability mode | Closure scope + permission lists | Closures + host wrappers + libc interception |
| **Granularity** | Per-fd rights (CAP_READ, CAP_WRITE, etc.) | Per-category patterns (tcp, fs_read, etc.) | Same (pattern-matched at layers 2-3) |
| **Revocation** | Implicit (close the fd) | Implicit (actor dies, context goes away) | Same (context dies; LD_PRELOAD unloaded) |
| **Nested restrictions** | Process forking; each child inherits restricted fds | Lexical nesting; each closure inherits parent's context | Same + host-language nesting |
| **Overhead** | Zero for allowed ops; syscall overhead for denied ops | Function-call overhead for permission checks | Minimal (LD_PRELOAD only on denied syscalls) |
| **Failure mode** | ENOTCAPABLE errno | Return code / exception | EACCES / EPERM (layer 3) overrides layer 1-2 |
| **OS coupling** | Tightly coupled to FreeBSD/POSIX | OS-agnostic (layer 1 only) | Requires POSIX + LD_PRELOAD (layer 3) |
| **Host language support** | Not applicable | Language-level only | Full support via host modules (layers 2-3) |
| **Privilege separation** | Casper daemon (separate process) | Actor delegation (same actor model) | Host modules delegate to parent actor |
| **Audit trail** | Not built-in; requires additional instrumentation | Not built-in; requires explicit logging | LD_PRELOAD can log all denied syscalls |
| **Portability** | FreeBSD only | All platforms | Linux/macOS/POSIX (layer 3); all platforms (layer 1) |

---

## Part 1.4 Aether's LD_PRELOAD Sandbox Layer: Syscall Interception

#### How It Works

Aether includes a third enforcement layer (`libaether_sandbox_preload.so`) that intercepts libc syscalls at runtime:

```c
// Grant file format (one per line)
tcp:*.example.com
fs_read:/app/data/*
fs_write:/tmp/*
env:HOME
env:PATH
*:*
```

When loaded via `LD_PRELOAD`, the library intercepts:

| Syscall | Interceptor | Checks |
|---------|-------------|--------|
| `open(path, flags)` | `open()` hook | fs_read / fs_write based on flags |
| `connect(fd, addr)` | `connect()` hook | tcp category + IP:port resource |
| `getenv(var)` | `getenv()` hook | env category + var name |
| `execve(path, args)` | `execve()` hook | exec category + command path |

**Key features:**

1. **Path resolution** — Resolves symlinks and `..` components before pattern-matching, preventing directory-traversal bypasses.
2. **Pattern matching** — Supports globs (`*`, `*suffix`, `prefix*`) and exact matches.
3. **IPv4-mapped IPv6 normalization** — Normalizes addresses like `::ffff:10.0.0.1` to `10.0.0.1` for consistent matching.
4. **Logging** — Can log all denied/allowed syscalls to a file or stderr (configured via `AETHER_SANDBOX_LOG`).
5. **Transparent to code** — The host language (Ruby, Python, Go) doesn't know it's being sandboxed; calls just fail gracefully.

#### Example: Ruby Sandboxing with LD_PRELOAD (and 7 other languages)

Aether supports **8 hosted languages**, each with the same three-layer protection:

**In-process languages** (use native interpreters):
- Ruby, Python, Perl, Lua, Tcl, JavaScript (Duktape)

**Out-of-process languages** (run as subprocesses under LD_PRELOAD):
- Go, Java (JVM via Panama FFI)

Example with Ruby:

```aether
import contrib.host.ruby

main() {
    // Create a sandbox context
    restricted = sandbox("worker") {
        grant_fs_read("/app/config.yaml")
        grant_tcp("api.example.com", 443)
    }
    
    // Ruby code runs with layer 1 (closure) + layer 2 (host module) + layer 3 (LD_PRELOAD) protection
    ruby.run_sandboxed(restricted, "
        File.read('/app/config.yaml')      # Allowed: matches fs_read:/app/config.yaml
        File.read('/etc/passwd')            # Denied by layer 3: EACCES
        TCPSocket.new('api.example.com', 443)  # Allowed
        TCPSocket.new('evil.com', 443)     # Denied by layer 3: EACCES
    ")
}
```

Same pattern works for Python, Perl, Lua, Tcl, JavaScript:

```aether
import contrib.host.python
python.run_sandboxed(restricted, "open('/app/config.yaml').read()")

import contrib.host.lua
lua.run_sandboxed(restricted, "io.open('/app/config.yaml'):read('*a')")

import contrib.host.perl
perl.run_sandboxed(restricted, "open(my $f, '<', '/app/config.yaml') or die")
```

**Defense in depth:**
- Layer 1: Guest code is a closure; can't access parent locals
- Layer 2: Host module scrubs ENV, $LOAD_PATH, installs permission checker
- Layer 3: Even if layers 1-2 are bypassed, LD_PRELOAD blocks the syscalls

---

## Part 2: Architectural Parallels

### 2.1 Common Principles

Both systems are built on **three shared insights:**

#### 1. Capabilities over Permissions

Both Capsicum and Aether use **capability-based access control** instead of traditional role-based access control (RBAC):

- **RBAC** (Unix permissions): Check the caller's identity/role. "User alice is in group developers, so she can read /home/projects/*"
- **Capability** (Capsicum/Aether): Check what the caller has been given. "This process has a read-only fd to /home/projects/foo; it can read that one file, nothing else."

The capability model is **more restrictive and composable**. You can hand off a capability without expanding the recipient's overall privileges.

#### 2. Containment Boundaries

Both systems enforce a **container/contained boundary**: The container can see the contained, but the contained cannot reach the container.

- **Capsicum:** A privileged parent process starts, opens fds, restricts them, and enters capability mode. The restricted process can't open new fds or break back out.
- **Aether:** A parent function creates a permission context, passes it to a closure. The closure can't reach parent locals or dynamically add permissions.

This principle appears in:
- Virtual machines (hypervisor sees guest, guest can't see hypervisor)
- Docker (host sees container, container can't see host)
- Java ClassLoader hierarchies (parent loader visible to children, not vice versa)

#### 3. Fail-Safe Defaults

Both default to **deny** rather than **allow**:

- **Capsicum:** Capability mode denies all global namespace access. You must explicitly pass fds.
- **Aether:** When embedded in a host application (hosted modules), capabilities are **empty by default**. You must explicitly grant them.

This inverts the traditional Unix model (allow by default, explicitly restrict), making security the default rather than an afterthought.

---

### 2.2 Where They Differ

#### 1. Scope of Enforcement

- **Capsicum:** Enforces at the **OS boundary** (syscall interface). A compromised process can't call `open()`, `socket()`, or `execve()` regardless of what code it's running.
- **Aether:** Enforces at the **language boundary** (compiler + runtime). A compromised actor can still call C externs or skip permission checks if the code is malicious.

**Implication:** Aether is better for *controlled* environments (DSL scripts, plugin systems, hosted configs); Capsicum is better for *untrusted* code (third-party binaries, legacy applications).

#### 2. Granularity

- **Capsicum:** File descriptors are the unit of isolation. Restricted rights apply to one fd.
- **Aether:** Categories and patterns are the unit. "tcp", "fs_read:*/tmp/*", "exec:bash" are examples.

**Implication:** Capsicum is finer-grained for file I/O; Aether is simpler for abstract resources.

#### 3. Performance

- **Capsicum:** Zero overhead for allowed operations. Denied operations are caught by the kernel.
- **Aether:** Permission checks are function calls. Not free, but not expensive (cache-friendly).

**Implication:** Capsicum is suitable for performance-critical code (tcpdump, database servers); Aether is fine for configuration/wiring code.

---

## Part 3: Opportunities for Aether on FreeBSD

### 3.1 A Fourth Layer: Capsicum as Process-Level Enforcement

**Current state:** Aether has three layers (language, host module, LD_PRELOAD).

**Opportunity:** Add a fourth layer on FreeBSD using Capsicum for process-level enforcement, enabling truly bulletproof sandboxing.

**Vision:** An Aether runtime that optionally leverages Capsicum as a fourth layer on FreeBSD, combining:
- Layer 1: Language-level isolation (closures)
- Layer 2: Host-language integration (Ruby, Python, Perl, Lua, Tcl, JS, Go, Java)
- Layer 3: LD_PRELOAD syscall interception (all POSIX systems)
- **Layer 4: Capsicum file-descriptor capabilities (FreeBSD only)**

#### Design: Four Layers of Defense

```
+============================ Layer 1 ============================+
| Aether Code (native or closure)                                 |
| - Closures can't reach parent locals (compiler-enforced)       |
+================================================================+
          |
          v
+========== Layer 2: Host Language Integration (8 langs) ==========+
| Ruby, Python, Perl, Lua, Tcl, JavaScript, Go, Java             |
| In-process: Ruby, Python, Perl, Lua, Tcl, JS                   |
| Out-of-process: Go (LD_PRELOAD), Java (JVM + Panama FFI)      |
| - Host module installs permission checker                      |
| - Scrubs runtime caches (ENV, LOAD_PATH, etc.)               |
+================================================================+
          |
          v
+============================ Layer 3 ============================+
| LD_PRELOAD Syscall Interception (all POSIX systems)           |
| - open(), connect(), execve(), getenv() hooks                 |
| - Path resolution (defeats symlinks, ..)                      |
| - Pattern matching (category:resource globs)                  |
| - EACCES/EPERM on denied syscalls                             |
+================================================================+
          |
          v
+============================ Layer 4 ============================+
| Capsicum (FreeBSD only, optional)                             |
| - File descriptor capability restrictions (CAP_READ, etc.)   |
| - cap_enter() to irreversibly enter capability mode           |
| - ENOTCAPABLE errno on denied operations                      |
+================================================================+
          |
          v
       [ Kernel ]
```

**Each layer is optional and stacks orthogonally:**
- Native Aether code: layer 1 is sufficient
- Hosted languages (Ruby, Python, Perl, Lua, Tcl, JS, Go, Java): layers 1-3 (layer 1 for calling code, layers 2-3 for guest code)
- On FreeBSD with Capsicum enabled: layers 1-4 for maximum defense

**How it works:**

1. **Aether compiler** generates actors with permission contexts (as it does today).
2. **Aether runtime** (new feature) detects if it's running on FreeBSD.
3. **If Capsicum is available:**
   - When an actor is spawned with a permission context, the runtime:
     - Opens the minimal set of file descriptors needed for that context
     - Uses `cap_rights_limit()` to restrict each fd to only the rights needed
     - Calls `cap_enter()` to enter capability mode
     - Spawns the actor in a child process with only those fds
   - The actor's language-level permission checks become an optional defense-in-depth layer.
4. **If Capsicum is not available:**
   - Fall back to language-level checks (as today).

#### Benefits

1. **Defense in depth** — Even if an actor is malicious and bypasses language-level checks, Capsicum blocks the syscall.
2. **Incremental** — Doesn't require rewriting Aether code; works with existing programs.
3. **Transparent** — Aether code doesn't need to know about Capsicum.
4. **Portable** — On non-FreeBSD systems, the language-level checks still work.

#### Example Flow

```aether
// Untrusted plugin (could be malicious)
plugin = |ctx| {
    // This is sandboxed: permission checks will deny it
    sandboxed_tcp_connect(ctx, "evil.com", 443)  // Denied by language check
    
    // But even if malicious, on FreeBSD with Capsicum:
    // direct_syscall_socket()  // Would be caught by Capsicum; ENOTCAPABLE
}

main() {
    trusted_ctx = list.new()
    list.add(trusted_ctx, "tcp")
    list.add(trusted_ctx, "db.internal")
    
    // On FreeBSD with Capsicum:
    // - Aether runtime opens TCP socket to db.internal
    // - Restricts fd with CAP_READ | CAP_WRITE
    // - Enters capability mode in a child process
    // - Spawns the actor with only that fd
    
    spawn_with_context(plugin, trusted_ctx)
}
```

---

### 3.2 Concrete Implementation Roadmap

#### Phase 1: Capsicum Detection & API Bindings (Low effort)

Create `std.capsicum` module with:

```aether
// Check if Capsicum is available
capsicum_available() -> int

// Enter capability mode (irreversible)
capsicum_enter() -> int

// Restrict a file descriptor's rights
capsicum_limit_rights(fd: int, rights: int) -> int

// Create a process descriptor
capsicum_create_process_descriptor(pid: int) -> int

// Query a process's Capsicum state
capsicum_is_in_capability_mode(fd: int) -> int
```

**Benefit:** Allows hand-written Aether code to leverage Capsicum today.

#### Phase 2: Runtime Integration (Medium effort)

Modify the actor spawning code in `runtime/scheduler/actor_pool.c`:

1. When `spawn_with_context()` is called, pass the permission context to the runtime.
2. If Capsicum is available:
   - Map the permission context to file descriptors and rights:
     - "tcp" + "db.internal:5432" → TCP fd to db.internal:5432 with CAP_READ | CAP_WRITE
     - "fs_read" + "/tmp/*" → Directory fd to /tmp with CAP_READ | CAP_LOOKUP
     - "exec" + "/bin/sh" → Not applicable; deny in capability mode
3. Open the minimal fds in the parent (privileged) process.
4. Fork a child process.
5. In the child, call `cap_rights_limit()` on each fd, then `cap_enter()`.
6. Execute the actor's code with only those fds available.

**Benefit:** Transparent Capsicum integration; existing Aether code gains OS-level enforcement on FreeBSD.

#### Phase 3: Capability Audit Logging (Medium effort)

Add optional instrumentation:

```aether
// Log when a permission is granted
audit_grant(actor_id: string, category: string, resource: string)

// Log when a permission check fails
audit_deny(actor_id: string, category: string, resource: string)

// Retrieve audit log (queryable at runtime)
audit_log() -> ptr
```

**Benefit:** Forensics and debugging; understand what each actor is doing.

#### Phase 4: Cross-Language Capsicum Callbacks (High effort, speculative)

Allow hosted modules (all 8 languages: Ruby, Python, Perl, Lua, Tcl, JS, Go, Java) to declare Capsicum requirements:

```aether
// Aether code compiled to library, embedded in Java
config = |ctx| {
    grant_tcp(ctx, "api.example.com", 443)
    grant_fs_read(ctx, "/etc/app/config")
    
    // Metadata for host to understand Capsicum needs
    // (requires new compiler emit mode)
}
```

The Java host could:
1. Parse the Aether library's metadata.
2. Understand that this Aether config needs tcp + fs_read.
3. Fork a child process, open those fds, apply Capsicum restrictions.
4. Load the Aether library in the restricted child.

**Benefit:** Aether configs run under OS-enforced sandboxing when embedded.

---

### 3.3 FreeBSD-Specific Optimizations

#### 1. Casper Daemon Integration

FreeBSD includes Casper, a daemon that provides services (DNS, syslog, pwd, grp) to capability-mode processes.

**Opportunity:** Aether actors running in Capsicum mode could delegate privileged operations to Casper instead of embedding them.

```aether
// Today: actor needs dns permission
sandboxed_resolve_hostname(ctx, "example.com")

// With Casper: actor has zero network rights, but can query Casper
capsicum_casper_gethostbyname("example.com")  // Delegated to Casper daemon
```

This further reduces what an actor can do directly.

#### 2. Jail Integration

FreeBSD jails are lightweight OS-level containers. An Aether actor could be spawned in a jail with:
- Restricted filesystem (chroot)
- Network isolation (no access to host network stack)
- Process isolation (can't see other jails)

**Opportunity:** Aether runtime could optionally spawn actors in jails for maximum isolation.

#### 3. RCTL (Resource Control)

FreeBSD's RCTL allows resource limits (CPU, memory, file descriptors) per process.

**Opportunity:** Aether's permission context could map to RCTL rules:

```aether
grant_memory(ctx, 100)     // 100 MB limit
grant_cpu(ctx, 500)        // 500 ms per second
grant_fds(ctx, 10)         // Max 10 open fds
```

The runtime would apply RCTL rules when spawning the actor.

---

## Part 4: Real-World Use Cases for Aether on FreeBSD

### 4.1 Secure Configuration Management

**Scenario:** A system administration tool (like Ansible or Puppet) uses Aether for configuration logic.

```aether
// Untrusted configuration script
config = |ctx| {
    // Script can only access /etc/myapp/ and connect to config.internal
    grant_fs_read(ctx, "/etc/myapp/*")
    grant_tcp(ctx, "config.internal", 8080)
    
    // Run the configuration logic
    fetch_config("config.internal:8080")
    parse_and_apply("/etc/myapp/app.conf")
}

main() {
    spawn_sandboxed(config)
}
```

**FreeBSD benefit:** Aether runtime spawns the config actor in:
- Capability mode (no access to /etc/passwd, /etc/shadow, network sockets)
- A chroot jail (filesystem isolation)
- RCTL limits (max memory, CPU)

Even if the config script is malicious or compromised, it can't escape its sandbox.

### 4.2 Plugin System for Services

**Scenario:** A database server or web server allows plugins written in Aether.

```aether
// User-supplied plugin
plugin = |ctx| {
    // Plugin can query the database and read data files
    grant_tcp(ctx, "db.internal", 5432)
    grant_fs_read(ctx, "/var/lib/myapp/data/*")
    
    // Cannot write, execute, or access other resources
    // ...plugin code...
}

main() {
    spawn_sandboxed(plugin)
}
```

**FreeBSD benefit:** Plugins run under Capsicum restrictions. A malicious plugin can't:
- Write to the filesystem (open file is read-only)
- Execute commands (no /bin/sh fd)
- Access the database beyond the allowed connection (socket fd is restricted)
- Fork (in capability mode)

### 4.3 Secure Scripting in System Tools

**Scenario:** A DevOps tool (like Terraform or Nomad) uses Aether for provisioning scripts.

```aether
// Provisioning script
script = |ctx| {
    grant_tcp(ctx, "*.cloud.provider", 443)        // Only to cloud provider
    grant_fs_read(ctx, "/var/lib/provisioning/*")
    grant_fs_write(ctx, "/tmp/provision-*")         // Scratch space
    grant_exec(ctx, "/usr/sbin/pw")                 // User management only
    
    // Create users, deploy services, etc.
    // Cannot access network outside cloud provider
    // Cannot read system files
    // Cannot execute arbitrary commands
}

main() {
    spawn_sandboxed(script)
}
```

**FreeBSD benefit:** Multiple scripts can run concurrently, each in its own jail with its own RCTL limits, completely isolated.

---

## Part 5: Challenges and Limitations

### 5.1 Capsicum Limitations

1. **Process-based only** — Capsicum sandboxing is per-process. Aether actors are lightweight; spawning a process per actor is heavy.
   - **Solution:** Use thread-based actors with Capsicum process boundaries only at higher levels (e.g., one process per actor pool).

2. **Syscall overhead** — Every syscall in capability mode is checked by the kernel. High-syscall-rate code might see overhead.
   - **Solution:** Batch operations, use sendfile/splice for zero-copy I/O, avoid per-request syscalls.

3. **Not portable** — Capsicum is FreeBSD-specific. Code needs to fall back gracefully on Linux/macOS/Windows.
   - **Solution:** Aether's language-level checks always work; Capsicum is opt-in and transparent.

4. **Complexity** — Understanding fd rights, capability mode, and Casper requires FreeBSD knowledge.
   - **Solution:** Wrap Capsicum in Aether std library; users write portable Aether code, runtime handles OS differences.

### 5.2 Aether Limitations

1. **Untrusted code can still bypass checks** — If an actor is malicious and compiled with the Aether code, it can skip permission checks.
   - **Mitigation:** On FreeBSD, use Capsicum for ultimate enforcement. Don't rely on language-level checks alone for untrusted code.

2. **No built-in audit trail** — Currently, permission checks don't log anything.
   - **Mitigation:** Phase 3 (audit logging) addresses this.

3. **No resource limits** — Aether can restrict *what*, not *how much* (bandwidth, memory, CPU).
   - **Mitigation:** On FreeBSD, use RCTL for resource limits.

---

## Part 6: Roadmap for Aether on FreeBSD

### Short Term (2-3 months)

1. **Add `std.capsicum` module** with basic bindings.
2. **Documentation** explaining how to use Capsicum from Aether.
3. **Examples** showing Capsicum-enabled plugins and sandboxed actors.

### Medium Term (3-6 months)

1. **Runtime integration** — `spawn_with_context()` optionally uses Capsicum.
2. **Automatic fd management** — Runtime maps permission contexts to fds and rights.
3. **CI on FreeBSD** — Test suite runs on FreeBSD; catch OS-specific bugs.

### Long Term (6+ months)

1. **Audit logging** — Built-in forensics.
2. **Casper integration** — Actor delegation to Casper daemon.
3. **Jail + RCTL support** — Multi-level sandboxing.
4. **Cross-language Capsicum metadata** — Hosted modules declare Capsicum needs.

---

## Part 7: Conclusion

**Capsicum and Aether are orthogonal strengths:**

- **Capsicum** is OS-enforced, fine-grained, and ironclad—but process-heavy and OS-specific.
- **Aether** is language-level, portable, and lightweight—but relies on code-level enforcement.

**A hybrid approach** (Aether code + Capsicum enforcement on FreeBSD) combines the best of both:

1. **Portability** — Aether code runs on any OS.
2. **Simplicity** — Users write high-level permission contexts; the runtime handles the details.
3. **Security depth** — Language-level checks catch bugs; Capsicum catches malice.
4. **Performance** — Fast path on allowed operations; Capsicum only on denied syscalls.

**For Aether to become a credible sandboxing platform on FreeBSD**, the roadmap is:

1. **Phase 1:** Capsicum bindings (lets hand-written Aether code use Capsicum).
2. **Phase 2:** Runtime integration (transparent Capsicum for actors).
3. **Phase 3:** Audit logging (forensics and debugging).
4. **Phase 4:** Ecosystem integration (Casper, jails, RCTL).

This makes Aether a unique proposition: **the only language-level sandbox model backed by OS-level enforcement when available**.

---

## References

### Capsicum Documentation

- [Capsicum: practical capabilities for UNIX](https://www.cl.cam.ac.uk/research/security/capsicum/) — Original research
- [FreeBSD Capsicum man pages](https://www.freebsd.org/cgi/man.cgi?capsicum) — `cap_enter(2)`, `cap_rights_limit(2)`, etc.
- [Casper daemon documentation](https://www.freebsd.org/cgi/man.cgi?casper) — Inter-process privilege delegation
- [Chromium Capsicum integration](https://www.chromium.org/) — Production use case

### Aether Documentation

- [`docs/containment-sandbox.md`](./containment-sandbox.md) — Aether's permission model and examples
- [`docs/aether-embedded-in-host-applications.md`](./aether-embedded-in-host-applications.md) — Hosted modules and capability-empty defaults
- [`docs/closures-and-builder-dsl.md`](./closures-and-builder-dsl.md) — Closure isolation and scope hygiene
- [`docs/emit-lib.md`](./emit-lib.md) — Embedding Aether as a library in host applications

### Related Security Research

- [USENIX Security 2010: Capsicum paper](http://www.trustedbsd.org/2010usenix-security-capsicum-website.pdf)
- [BSDCan 2014: Capsicum and Casper](https://www.bsdcan.org/2014/schedule/track/Security/486.en.html)
- [Oblivious Sandboxing with Capsicum](https://www.engr.mun.ca/~anderson/publications/2017/towards-oblivious-sandboxing.pdf)
