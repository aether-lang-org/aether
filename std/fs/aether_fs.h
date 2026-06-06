#ifndef AETHER_FS_H
#define AETHER_FS_H

#include <stddef.h>

// ---- Structured-error kinds (pilot — issue #392) ----
//
// Mirror of the KIND_* `const` block in std/fs/module.ae. Update both
// in lock-step. Translation from `errno` to one of these values is
// done by `aether_fs_errno_to_kind()` (see aether_fs.c) — that helper
// is the *only* errno→kind site, so the four pilot primitives
// (fs.copy, fs.move, fs.realpath, fs.chmod) all classify failures the
// same way.
#define AETHER_FS_KIND_OK                0
#define AETHER_FS_KIND_NOT_FOUND         1
#define AETHER_FS_KIND_PERMISSION_DENIED 2
#define AETHER_FS_KIND_EXISTS            3
#define AETHER_FS_KIND_CROSS_DEVICE      4
#define AETHER_FS_KIND_IO                5
#define AETHER_FS_KIND_INVALID           6
#define AETHER_FS_KIND_LOOP              7
#define AETHER_FS_KIND_NAME_TOO_LONG     8
#define AETHER_FS_KIND_NO_SPACE          9
#define AETHER_FS_KIND_IS_DIR           10
#define AETHER_FS_KIND_NOT_DIR          11
#define AETHER_FS_KIND_UNAVAILABLE      99

// File operations
typedef struct {
    void* handle;
    int is_open;
    const char* path;
} File;

File* file_open_raw(const char* path, const char* mode);
char* file_read_all_raw(File* file);
int file_write_raw(File* file, const char* data, int length);
int file_close(File* file);
int file_exists(const char* path);
/* Path-agnostic existence check: 1 if anything is at `path`
 * (regular file, directory, symlink, fifo, …), 0 otherwise.
 * Distinct from file_exists (regular-file-only) and dir_exists
 * (directory-only). Uses lstat so dangling symlinks count as
 * existing — matches POSIX `test -e`. */
int fs_path_exists(const char* path);
int file_delete_raw(const char* path);
int file_size_raw(const char* path);
int file_mtime(const char* path);

// Directory operations
int dir_exists(const char* path);
int dir_create_raw(const char* path);
int dir_delete_raw(const char* path);

// `mkdir -p` semantics: create `path` and any missing parent directories.
// Treats already-existing directories as success. Returns 1 on success,
// 0 on failure. Raw extern — use the `mkdir_p` Go-style wrapper in
// std/fs/module.ae from most code.
int fs_mkdir_p_raw(const char* path);

// Symbolic-link operations. The *_raw functions are the low-level
// externs; idiomatic callers use the Go-style `symlink` / `readlink` /
// `unlink` wrappers in std/fs/module.ae which return `(value, err)`.
//
// fs_symlink_raw: create a symlink at `link_path` pointing to `target`.
//                 Returns 1 on success, 0 on failure (e.g. link already
//                 exists). `target` is recorded verbatim — NOT resolved
//                 at create time, so a relative target stays relative.
//
// fs_readlink_raw: read a symlink. Returns a heap-allocated string
//                  containing the link target, or NULL if `path` is not
//                  a symlink (or cannot be read). Caller frees.
//
// fs_is_symlink: returns 1 if `path` is itself a symlink (does not
//                follow), 0 otherwise. Pure boolean query — no wrapper
//                needed, matches file_exists / dir_exists shape.
//
// fs_unlink_raw: remove a file or symlink. Will NOT remove a directory
//                — use dir_delete_raw for that. Returns 1 on success,
//                0 on failure.
int   fs_symlink_raw(const char* target, const char* link_path);
char* fs_readlink_raw(const char* path);
int   fs_is_symlink(const char* path);
int   fs_unlink_raw(const char* path);

// Non-atomic binary write to `path` — opens "wb", writes exactly
// `length` bytes, closes. Binary-safe (embedded NULs OK) because
// the length is explicit. Simpler and cheaper than fs_write_atomic_raw
// when the caller doesn't need the write-to-tmp + fsync + rename
// dance — useful for scratch files, caches, or any write where a
// partial state on crash is acceptable. Returns 1 on success, 0 on
// any failure (open/write/close). On failure, whatever was written
// stays on disk — caller's responsibility to remove(2) the partial
// file if needed.
int fs_write_binary_raw(const char* path, const char* data, int length);

// Durable write to `path`: writes to `<path>.tmp.<pid>`, fsyncs, then
// renames over the destination. Survives a crash in the middle of a
// write without leaving a half-finished file at `path`. Takes a
// length so the input is binary-safe (embedded NULs OK). Returns 1
// on success, 0 on any failure (tmp open/write/fsync/rename). On
// failure the tmp file is removed so the caller doesn't leak.
int fs_write_atomic_raw(const char* path, const char* data, int length);

// Rename `from` to `to` — thin wrapper around POSIX rename(2). On
// POSIX rename(2) is atomic when source and target are on the same
// filesystem; callers composing with fs_write_atomic_raw should
// pick a tmp path on the same fs as the target. Returns 1 on
// success, 0 on failure.
int fs_rename_raw(const char* from, const char* to);

// Single-stat accessor. Writes the entry kind into *out_kind:
//   1 = file, 2 = directory, 3 = symlink, 4 = other (FIFO, socket,
//   device, ...). Size written to *out_size, mtime to *out_mtime.
// Uses lstat(2) — symlinks show up as kind 3, not followed.
// Returns 1 on success, 0 on failure (missing path, stat error).
// On failure all three out-params are zeroed.
int fs_stat_raw(const char* path, int* out_kind,
                int* out_size, int* out_mtime);

// Split-accessor pair for Aether callers that don't want to pass C
// out-parameters. fs_try_stat caches the most recent stat result in
// thread-local storage; the fs_get_* accessors read from it. Returns
// 1 on success, 0 on stat failure — in the failure case the getters
// all return 0. Typical Aether use:
//
//   if fs_try_stat(path) != 0 {
//       kind  = fs_get_stat_kind()
//       size  = fs_get_stat_size()
//       mtime = fs_get_stat_mtime()
//   }
int fs_try_stat(const char* path);
int fs_get_stat_kind(void);
int fs_get_stat_size(void);
int fs_get_stat_mtime(void);

// Read the entire file at `path` into a newly malloc'd buffer.
// Sibling of file_read_all_raw that is length-aware and binary-safe:
// the returned buffer contains exactly the file's bytes (including
// embedded NULs) and *out_len carries the byte count. A trailing
// '\0' is appended past *out_len as a convenience for pure-text
// callers, but it is NOT included in the length. Returns NULL on
// any failure (missing path, stat fail, read short). Caller frees.
char* fs_read_binary_raw(const char* path, int* out_len);

// Aether-friendly split accessor: fs_try_read_binary stashes the
// read result (buffer + length) in TLS and returns 1 on success /
// 0 on failure; fs_get_read_binary / fs_get_read_binary_length
// read from the cache. The cached buffer is freed on the next call
// to fs_try_read_binary (or when fs_release_read_binary is called
// explicitly), so callers should copy out before issuing the next
// read. Matches the shape of fs_try_stat + fs_get_stat_*.
int   fs_try_read_binary(const char* path);
const char* fs_get_read_binary(void);
int   fs_get_read_binary_length(void);
void  fs_release_read_binary(void);

/* The structured-error pilot primitives (#391 + #392) — fs_copy_raw,
 * fs_move_raw, fs_realpath_raw, fs_chmod_raw — return the codegen-
 * emitted (int, int, string) / (string, int, string) tuple ABI and
 * have no C-visible declarations in this header (they're only ever
 * called from generated Aether code, the same way fs_read_binary_tuple
 * is). The saturation contract is documented at each implementation
 * site in std/fs/aether_fs.c. */

// Path operations
char* path_join(const char* path1, const char* path2);
char* path_dirname(const char* path);
char* path_basename(const char* path);
char* path_extension(const char* path);
int path_is_absolute(const char* path);

// Lexical path ops (#632). Pure-string; never touch the filesystem.
// path_clean: Go filepath.Clean semantics (collapse `//`/`./`,
//             resolve `..` against a segment stack, preserve a leading
//             `/`, preserve unresolved leading `..` on relative paths).
//             Empty input → ".".
// path_is_within_base: lexical containment check — 1 iff cleaned
//             `target` lies under cleaned `base`. Security-critical:
//             the right pre-validation for blob stores / static
//             servers / archive extractors to reject path traversal
//             BEFORE open(2). Symlinks NOT followed.
// path_rel: relative path from `base` to `target` (Go filepath.Rel).
//             NULL when one is absolute and the other relative.
char* path_clean(const char* path);
int   path_is_within_base(const char* base, const char* target);
char* path_rel(const char* base, const char* target);

// Positional I/O (#640). All operate on a File* obtained via
// file_open_raw with a mode that admits both read and write ("r+",
// "w+", "a+"). Loops on short transfers; EINTR-safe on POSIX;
// _chsize_s / _commit on Windows.
long        fs_pwrite_raw(File* file, const char* data, int length, long offset);
int         fs_pread_raw(File* file, int length, long offset);
const char* fs_get_pread(void);
int         fs_get_pread_length(void);
void        fs_release_pread(void);
const char* fs_ftruncate_raw(File* file, long length);
const char* fs_fsync_raw(File* file);

// Directory listing
typedef struct {
    char** entries;
    int count;
} DirList;

DirList* dir_list_raw(const char* path);
int dir_list_count(DirList* list);
const char* dir_list_get(DirList* list, int index);
void dir_list_free(DirList* list);

// Glob: match files by pattern (e.g., "src/**/*.c")
// Returns a DirList with full paths of matching files.
DirList* fs_glob_raw(const char* pattern);

// Multi-pattern glob: takes a list of patterns, returns merged results.
// E.g., fs_glob_multi_raw(["**/*.c", "**/*.h"]) returns all .c and .h files.
DirList* fs_glob_multi_raw(void* pattern_list);

#endif // AETHER_FS_H

