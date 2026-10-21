# A write-back that fails partway lost the map it had written

Date: 2026-10-21. Base: `main` at `cceff596` (#425's merge). Found while
validating B4's second pull request (#426): `make BUILD=release
test-upgrade` on x86-64 failed in `test-pkgtx`'s `space` stage, `fsctl
check after the refusal and the install: no line matching '^clean$'`,
with `7 leaked blocks: 47357, 47358, 47359, 47360, 47361, 47362, 47363`.
Three more runs of `test-pkgtx` on the same image: two failed the same
way (7 leaked), one passed. The defect is in cosmofs on main, not in
#426, and it fails B4's acceptance test, so it is fixed here.

## Scope and baseline

`test-pkgtx` fills the installed root in synced steps until under
256 KiB is free. The last step's write-back fails: `vfs: 8 dirty page(s)
of inode 512 on cosmofs lost: write-back failed (-28)`. A copy of the
stage that runs `fsctl check` after every step (on an install that went
through `test-pkgtx`'s earlier stages first) finds the blocks leaked
right after that step, 5 and 3 blocks in two runs, ending at 47363; on a
fresh install, two runs stayed clean, one of them with the same
write-back failure.

## Mechanism

`cfs_writepages` writes a 32 KiB record through `record_write`, whose
plain fallback calls `data_write_block` for each of the eight blocks.
Each one maps its new block in the open inode and queues the block it
replaces free. When the fifth finds no room, the first four are mapped
in the open inode -- and nowhere else: `cfs_writepages` and
`cfs_writepage` called `inode_sync` only when the write succeeded. The
pages stay dirty for a retry. When the vnode is released first instead
(`cat` closes the file; the release's own write-back fails again and
`pagecache_drop` drops the pages), `cfs_evict` stores nothing for a
named inode, and the map goes with the vnode. The next commit then has
blocks no inode names (leaked) and, for a rewrite, frees the blocks the
committed inode still names (free in use). In `test-pkgtx` the file is
new, so only the leak shows.

Forced: the self-test `cosmofs-extent-fail` gains an `evict` case. It
rewrites a committed 16-page file, fails the fifth `set_extent` of the
write-back (`FI_COSMOFS_EXTENT_NOMEM`, then `FBIG`), releases the file
with every `set_extent` failing (the release's attempt is the second
hit), commits, and runs `cosmofs_check`. Without the fix: `6 leaked, 6
free-in-use` on both architectures, where four of each would be the
data blocks alone; the other two were not identified.

## Change

- `kernel-services/filesystem/cosmofs/cosmofs.c`: `sync_after_write`,
  called by `cfs_writepage` and `cfs_writepages` after the write whether
  it succeeded or not: the inode is stored, so the blocks the write
  mapped are its own and the ones they replaced are free only once the
  inode no longer names them. What was written is the new contents
  under their own checksums (CF16), so it is kept; the pages left dirty
  are written by the retry. An abandoned transaction (`fs->failed`)
  stores nothing.
- `kernel-services/filesystem/cosmofs/cosmofstest.c`: the `evict` case;
  after a remount each page is the old contents or the new, and the
  first four are new.
- CF17 names the site; `testing.md` the case.
- `tools/cosmofs-partial-write-probe.py`: `--old` reverse-applies the
  fix commit's `cosmofs.c` in a throwaway worktree.

## Validation

`tools/cosmofs-partial-write-probe.py`, both architectures: `--old`
fails `rep.alloc_not_seen.count == 0 && rep.seen_not_alloc.count == 0`
with `evict: release hits 2; 6 leaked, 6 free-in-use`; fixed, `0
leaked, 0 free-in-use` in both `evict` runs (`-ENOMEM`, `-EFBIG`) and
`cosmofs-extent-fail ... ok`.

The validation matrix on this commit, every step PASS: x86-64
`host-test` 83 s, `fuzz` 62 s, `analyze` 230 s, debug boots at
`QEMU_SMP=1/2/4` 253 / 154 / 162 s, `test-smp2` 162 s, `test-chaos`
190 s, `test-harness-retry` 189 s, release 50 s; AArch64 `host-test`
83 s, `analyze` 228 s, debug boots 267 / 156 / 172 s, `test-smp2` 157 s,
`test-chaos` 196 s, `test-harness-retry` 199 s, release 56 s.

`make BUILD=release test-pkgtx` three times on each architecture, and
`test-upgrade` (which runs it again) once: all PASS. One x86-64 run's
fill hit the write-back failure (`write-back failed (-28)` twice), the
case that leaked before, and its check was clean.
