# `ae run` exe cache: verify the published artifact (and give us `ae cache clear`)

**From:** the aether-ui line (2026-07-23) · **Where it bit:** winbaz
(Windows/MSYS2), running aether-ui's `tests/spec_matrix.sh` (44 suites,
each spec a separate `ae run` program).

## Symptom

Under bulk sequential `ae run` load on MSYS2, a spec program
intermittently dies at exec with:

    Program crashed (signal 1)

(`tools/ae.c` ~3317's `rc < 0` report — on Windows this is the mapped
"process failed to start/died at startup" case, not a POSIX signal.)

Two distinct failure shapes composed:

1. **Roaming flake** — freshly compiled, freshly published exes
   sometimes die on their FIRST exec (AV scan / spawn pressure). Which
   suite dies scrambles run to run; re-running alone is green.
2. **Pinned failure** — occasionally the flake strikes in a way that
   leaves the CACHED exe at `~/.aether/cache/<key>.exe` bad, after
   which that spec fails **deterministically on every future run**,
   including quiet single runs. Evicting the entry (or pointing
   `AETHER_CACHE_DIR` at a fresh dir) immediately restores green — same
   source, same toolchain, so it's the cached artifact, not the code.

We could not catch the pinned artifact "in the act" to say exactly what
is wrong with it (by the time we inspected, sizes looked plausible),
but the fresh-cache-dir A/B is conclusive: bad entry, silently served
forever.

## Asks

1. **Verify on publish.** After `cache_publish`'s rename, re-hash the
   published file (the FNV-64 machinery is already there) against the
   just-built temp's hash; on mismatch, delete the entry and fall back
   to the private-temp-exe path (which already exists for rename
   failure). Cheap, and turns "pinned forever" into "one slow run".
2. **Verify on use.** Optionally store the exe's own content hash
   beside the entry (`<key>.sum`) and check it before exec'ing a cached
   exe — catches post-publish corruption/AV quarantine-mangling too.
3. **`ae cache clear` (and `ae cache dir`).** Today the remedy is
   knowing the undocumented `~/.aether/cache` path and hand-deleting.
   A first-class subcommand makes the escape hatch discoverable —
   support already half-exists via `AETHER_CACHE_DIR`.
4. (Nice-to-have) On the `rc < 0` report, print the exe path that was
   run — "Program crashed (signal 1)" alone gives no thread to pull
   when the cache is the suspect.

## Workarounds deployed in aether-ui meanwhile

`tests/spec_matrix.sh` retries the "spec died before reporting
anything" class once per suite (assertion failures never retry), and we
purged winbaz's cache by mtime. Both are mitigations, not fixes — the
pinned-entry class defeats retries until the cache is cleared by hand.
