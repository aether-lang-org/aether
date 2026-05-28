#ifndef AETHER_OS_H
#define AETHER_OS_H

#include <stdint.h>

// Run a shell command, return exit code
int os_system(const char* cmd);

// Run a command and capture stdout as a string
// Returns heap-allocated string (caller must free), or NULL on failure
char* os_exec_raw(const char* cmd);

// Get environment variable, returns string or NULL if not set
char* os_getenv(const char* name);

// Replace the current process image with `prog`, passing each element
// of `argv_list` as an argv entry. `argv_list` is an Aether `list<ptr>`
// whose entries must be C strings; element 0 is argv[0] for the new
// program and need not match `prog`. Does NOT return on success — on
// failure returns -1 and leaves the current process running. `prog` is
// looked up on PATH if it does not contain a slash (POSIX `execvp`).
// Not available on Windows (returns -1).
int os_execv(const char* prog, void* argv_list);

// Search PATH for an executable named `name`. Returns the absolute
// path to the first executable hit, or NULL if not found. If `name`
// already contains '/', it's returned as-is when it's executable
// (matches POSIX `command -v` semantics for absolute/relative paths).
// Caller owns the returned string.
char* os_which(const char* name);

// Run a child process directly via fork+execvp+waitpid (POSIX) or
// CreateProcessW (Windows — TODO). NO SHELL is interpreted: argv items
// are passed verbatim, so paths-with-spaces, $vars, |, ;, *, etc. in
// arguments are not metachars. Returns the child's exit code, or -1
// on failure to spawn (program not found, etc.).
//
//   prog    — program to execute. Resolved via PATH if it has no '/'.
//   argv    — Aether list (ArrayList*) of strings; argv[0] should be the
//             program name itself. May be NULL (treated as empty list).
//   env     — Aether list (ArrayList*) of "KEY=VALUE" strings, or NULL
//             to inherit the parent's environment.
int os_run(const char* prog, void* argv, void* env);

// Same as os_run but captures the child's stdout into a heap-allocated
// string the caller must free. Returns NULL on spawn failure. The
// child's exit code is discarded — use `os_run_capture_status_raw`
// below if you need both stdout and the exit code.
char* os_run_capture_raw(const char* prog, void* argv, void* env);

// Tuple-returning sibling of os_run_capture_raw — captures stdout AND
// exposes the child's exit code. Issue #289.
//
// The C return shape is `_tuple_string_int_string` (same anonymous
// struct typedef the codegen synthesises from
// `extern os_run_capture_status_raw(...) -> (string, int, string)`).
// The function itself is declared by the codegen-emitted typedef in
// every translation unit that imports `std.os`; this header omits a
// prototype to avoid duplicating the typedef across two definition
// sites that don't see each other's tag.
//
// Layout: (stdout, status, err)
//   - stdout: heap-allocated capture (caller frees), or "" on failure.
//   - status: WEXITSTATUS on normal exit; -1 when killed by signal or
//             when the spawn itself failed.
//   - err:    "" on successful spawn (regardless of exit code);
//             non-empty when the fork/exec couldn't run.

// Current UTC time as an ISO-8601 timestamp: "YYYY-MM-DDThh:mm:ssZ"
// (20 chars + NUL). Returns a heap-allocated string the caller must
// free. On clock / format failure returns an empty string (never NULL).
// Thread-safe via gmtime_r / gmtime_s.
//
// The `_raw` suffix matches the os_exec_raw / os_run_capture_raw
// pattern: Aether callers go through the `now_utc_iso8601()` wrapper
// in module.ae; the raw name keeps the extern from colliding with
// that wrapper's mangled C symbol.
//
// Shape is deliberately the one thing downstream code needs for
// "time this event happened" fields (Subversion's rev-blob date,
// JSON response timestamps, log-line prefixes). Sub-second precision,
// timezone offsets, and strftime-style format flags are out of scope
// for v1; additive variants (now_utc_iso8601_ms_raw, format_time) can
// land without disturbing this one.
char* os_now_utc_iso8601_raw(void);

// Process identifier for the current process. Useful for tmpfile
// names, per-process locks, log prefixes. POSIX getpid(2); Windows
// _getpid(). Returns 0 on platforms without filesystem (no-op stub).
int os_getpid_raw(void);

// Wall-clock time as the two fields of POSIX struct timeval: whole
// seconds since the Unix epoch, and the sub-second microsecond
// fraction (0..999999). POSIX gettimeofday(2); Windows
// GetSystemTimeAsFileTime. Both return 0 on no-filesystem builds.
int64_t os_wall_seconds_raw(void);
int     os_wall_micros_raw(void);

// Monotonic clock — invariant across NTP adjustments. Value-domain is
// opaque (epoch is boot / process-start / arbitrary); only *deltas*
// are meaningful. Use for animation tick loops, frame-time budgets,
// elapsed-time measurement, anything where wall-clock jumps would
// corrupt the answer.
//
// Return type is int64_t (NOT C `long`): Aether's `long` is 64-bit on
// every platform, but C `long` is 32-bit on Windows (LLP64). Returning
// C `long` would truncate to 4 bytes in the Aether 8-byte slot — the
// exact hazard PR #562 fixed for string_to_long_raw.
//
// Implementation: clock_gettime(CLOCK_MONOTONIC) on POSIX,
// QueryPerformanceCounter on Windows. Both return 0 on
// no-filesystem builds.
int64_t os_now_monotonic_ms_raw(void);
int64_t os_now_monotonic_ns_raw(void);

#endif
