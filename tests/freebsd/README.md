# FreeBSD-only sandbox tests

These exercise **real Capsicum / Casper enforcement** and can only pass on a
FreeBSD host with a Capsicum-enabled kernel. GitHub's CI matrix has **no
FreeBSD runner**, so they live here (outside `tests/regression/`) and are run
by `run.sh` / the nightly harness rather than the normal test suite.

The portable side of the contract — graceful degrade off FreeBSD, the gate
`aeo`'s `require_capsicum()` / `prefer_capsicum()` depends on — **is** covered
by `tests/regression/test_capsicum_portability.ae`, which runs on every CI
platform.

## Tests

- **`capsicum_enforce.ae`** — enters capability mode (irreversible, so the
  whole program is one unit), asserts `in_mode()` flips 0→1 and that opening a
  global-namespace path (`/etc/passwd`) is **denied** afterward (ECAPMODE).
- **`casper_delegation.ae`** — the two-phase pattern: open a `system.pwd`
  Casper channel **before** `capsicum.enter()`, then resolve `root → uid 0`
  **after** entry (impossible without Casper, since capability mode blocks
  `/etc/passwd`). Deterministic; no network.

Each `.ae` exits **0 = pass, 1 = fail, 2 = skip (not FreeBSD)**.

## Running

```sh
sh tests/freebsd/run.sh           # uses ./build/ae; all-skip + exit 0 off FreeBSD
AE=/path/to/ae sh tests/freebsd/run.sh
```

## Nightly self-test (the CI stand-in)

`nightly.sh` syncs the target branch, builds from clean, runs these tests +
the portable contract test + the `#668` seccomp clone-fence test, and **files
a GitHub issue on failure** (deduped by an issue-title marker; comments on the
existing issue rather than spamming new ones). Intended for a cron on the
GhostBSD build box.

```sh
# GhostBSD cron (03:17 nightly):
17 3 * * *  cd $HOME/aether && BRANCH=feat/freebsd-sandbox-parity-v2 \
            sh tests/freebsd/nightly.sh >> $HOME/aether-nightly.log 2>&1
```

`gh` must be authenticated on the host for issue filing; without it the script
still runs the tests and just prints the report. `NO_ISSUE=1` forces a dry run.

When a real FreeBSD CI runner exists, fold `run.sh` into the matrix and retire
the nightly (or keep it as defence in depth).
