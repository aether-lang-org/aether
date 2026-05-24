# Note for Nic — CHANGELOG version drift from the release workflow

**Date:** 2026-05-24
**TL;DR:** The `## [current]` → `## [X.Y.Z]` rename in the release
automation is fragile when `[current]` is missing at tag time. It has
already mislabeled two releases' notes (0.177 and 0.180). I've corrected
the CHANGELOG **in the working tree only** (uncommitted, on `main`) for
those two; the workflow itself still needs a guard, and older releases
may have the same drift. Over to you on how/whether to land.

---

## What's wrong

Two recent releases' CHANGELOG entries ended up under the wrong version
header, and two `## [X.Y.Z]` sections were missing entirely:

1. **0.180.0** had **no `## [0.180.0]` section**. The one thing that
   shipped in it — the "trailing commas tolerated in `exports (…)`" fix
   (PR #541) — was filed under `## [0.179.0]`.
2. **0.177.0** had **no `## [0.177.0]` section**. Its three entries
   (`std.mem.get_byte_sz`, `proxy.mount_methods`, `@c_import` struct
   pointers) were absorbed into `## [0.178.0]`.

Evidence (anyone can reproduce):

```sh
# 0.180 tag still shows [0.179.0] at the top — no [0.180.0] was ever cut:
git show v0.180.0:CHANGELOG.md | grep -m1 '^## \['        # => ## [0.179.0]
# the trailing-comma entry was added AFTER the v0.179.0 tag (so it's 0.180 content):
git diff v0.179.0 v0.180.0 -- CHANGELOG.md                # one + hunk: trailing-comma

# 0.178 release RENAMED the existing [0.177.0] header instead of adding a new one:
git diff v0.177.0 v0.178.0 -- CHANGELOG.md | grep '^[+-]## \['
#   -## [0.177.0]
#   +## [0.178.0]
```

## Root cause

`.github/workflows/release.yml` renames `## [current]` → `## [X.Y.Z]` at
tag time. That step assumes a `[current]` section exists. When it
doesn't, the behavior is inconsistent and wrong:

- The PR that landed between 0.179 and 0.180 added its entry **under the
  already-released `## [0.179.0]`** (CONTRIBUTING says "if `[current]` is
  missing, create it" — that step was skipped). At 0.180 release the
  rename found no `[current]` and silently produced **no** `[0.180.0]`.
- At 0.178 release, with no `[current]` present, the rename grabbed the
  **top existing header** (`[0.177.0]`) and relabeled it `[0.178.0]`,
  absorbing 0.177's notes.

So the same missing-`[current]` condition yields two different failure
modes (skip vs. clobber). It will recur on any release whose window had
no PR that created a fresh `[current]`.

## What I changed (working tree only — NOT committed/pushed)

`CHANGELOG.md` on `main`, `+36 / −25`:

- Created `## [0.180.0]` and moved the trailing-comma entry into it
  (out of `[0.179.0]`).
- Restored `## [0.177.0]` by splitting its three entries back out of
  `[0.178.0]`.
- Left `[0.179.0]` (C-variadic `va_list`, series-collapse) and the
  genuine `[0.178.0]` entries (address-of, `sizeof`/`offsetof`,
  builder-vs-function collision) where they belong — verified against the
  `v0.179.0` / `v0.178.0` tags.

Headers now run gapless `0.181 → 0.180 → 0.179 → 0.178 → 0.177 → 0.176`,
each matching what shipped at its tag. Content-only; no code touched.

**It is uncommitted on `main`** (Paul asked to keep it working-tree-only
and not commit to `main` directly). Decide how to land it — a `docs/`
branch + PR is the convention.

## Recommended follow-ups (yours to prioritize)

1. **Guard the workflow** so this can't recur. Options:
   - Before the rename, if `## [current]` is absent, **insert an empty
     one** at the top (so the rename always has a target and a release
     with no notes gets an empty section, not a clobbered one); or
   - **Fail the release** when `[current]` is missing, forcing a human to
     add notes; or
   - Have the rename match `## [current]` **only** (anchored) and never
     fall through to relabeling an existing `## [X.Y.Z]` header.
   I'd lean toward "insert empty `[current]` if missing" — it's the least
   surprising and keeps releases unblocked.
2. **Older gaps** I did not touch (scope was 0.180 + 0.177): the headers
   also skip `0.171`, `0.169`, `0.165`, … Some are probably genuinely
   empty releases; a couple may be the same clobber. A full
   tag-vs-section reconciliation (`for t in $(git tag -l 'v0.1*'); do …`)
   would confirm — happy to run it if you want the complete picture.

— left by Paul's Claude session while reconciling the 0.180/0.179 notes.
