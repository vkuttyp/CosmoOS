# The release kernel named another release — 2026-10-21

Found by v1.0.8's release (roadmap 1.2 B4's last step). Base: main
`d7688f55`. Branch `release-describe`.

## The defect

The annotated tag `v1.0.8` on `d7688f55` (main CI green, run
39548096676) started release run 39553838688. All four `build` jobs
failed at "The set says the tag":

```
check-release-version: cosmoos-x86_64-kernel.elf: build id v1.0.7, v1.0.8, not v1.0.8
check-release-version: cosmoos-aarch64-kernel.elf: build id v1.0.7, v1.0.8, not v1.0.8
```

`check-release-version.py` requires every git-describe-shaped string in
the kernel to be the tag (`docs/development.md`, "Releases"). B1 added
a cosmofs message, `cosmofs_snap.c`, logged when a snapshot raises a
pool to format 12:

```
cosmofs: format version 11 -> 12 (snapshot identity, %u snapshot(s)); a kernel that reads format 11 at most (v1.0.7 and older) cannot mount this pool
```

Its `v1.0.7` is describe-shaped. A built AArch64 release kernel holds
two such strings: the banner's build id (`v1.0.7-24-g5c7b507f-dirty` in
a local build) and this one. No other kernel source string matches the
pattern. Main's CI never ran the check, which only the release workflow
did, so nothing failed before the tag.

## Changes

- The message says "(CosmoOS 1.0.7 and older)", which is not
  describe-shaped; the guide's snapshots page and the cosmofs design quote
  it so.
- `check-release-version.py --kernel ELF --build-id ID`: the kernel holds
  no describe-shaped string but ID (none when ID is a bare hash).
- CI's "The release kernel names no other build id", after "Build and
  boot test (release)" on both architectures, runs it with `git describe
  --always --dirty`.
- `tests/host/test_release_version.py`: the `--kernel` mode on synthetic
  kernels (the old message fails, named; the new one passes; a bare hash;
  `--kernel` with `--tag` refused).

## Proof

`tools/release-describe-probe.py [--old] [--arch A]` builds REV's release
kernel in a throwaway worktree and runs the check against the worktree's
describe. `--old` reverse-applies the fix commit's `cosmofs_snap.c`.

| arch | fixed | old |
|---|---|---|
| x86_64 | PASS: only `v1.0.8-1-g8e45a335` | PASS: the kernel also says `v1.0.7` |
| aarch64 | PASS: only `v1.0.8-1-g8e45a335` | PASS: the kernel also says `v1.0.7` |

## The release

The tag `v1.0.8` produced no release (the `GitHub release` and
repository jobs were skipped). It moves to main's merge of this change,
and the release runs from there.
