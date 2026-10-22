#!/usr/bin/env python3
"""Prove roadmap 1.2 B3's record of what a copy missed (CF25) and that a
write during a resilver reaches the copy being rebuilt (CF28).

    b3-copystate-probe.py --case NAME [--old] [--arch x86_64|aarch64] [--tree REV]

The record is a new feature, not a fix, so there is no old commit to
revert: --old removes one rule of it in a throwaway worktree, by the
replacement the case names, and runs a debug `make test`:

  premark  the commit marks its own writes before it fills the record   cosmofs-copy-bitmap
           (cfs_copystate_fill): without it the record misses every
           block of the commit that wrote it
  persist  the commit writes the record at all (cfs_copystate_prepare):  cosmofs-copy-bitmap
           without it a remount knows nothing of what the copy missed
  resilver a write goes to a RESILVERING copy too (pool_write_flags):  cosmofs-resilver
           without it a block written during the resilver is never
           on the rebuilt copy

Old mode passes when the self-test fails at the case's check and nothing
else fails but the tests named as its consequences. Fixed mode passes when
`make test` passes. Logs stay under out/b3-copystate-probe/<case>-<arch>-<mode>.
Exit 0 when the mode shows what it should, else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MEMBER = "kernel-services/filesystem/cosmofs/cosmofs_member.c"
POOL = "kernel-services/storage/pool.c"

# file, the text the rule is, what --old leaves instead, self-test, the
# check its old run fails, other self-tests allowed to fail with it.
CASES = {
    "premark": (MEMBER,
                """        list_for_each_entry(it, &fs->bufs, link)
            if (it->dirty && CFS_DVA_VDEV(it->blkno) == v)
                pool_mark_missed(fs->pool, it->blkno);
        for (uint64_t blk = 0; blk < m->first_usable; blk++)
            pool_mark_missed(fs->pool, CFS_DVA(v, blk));""",
                """        (void)it;""",
                "cosmofs-copy-bitmap", r"m", ()),
    "persist": (MEMBER,
                """int cfs_copystate_prepare(struct cfs *fs)
{
    if (fs->rdonly)
        return 0;""",
                """int cfs_copystate_prepare(struct cfs *fs)
{
    if (fs || fs->rdonly)
        return 0;""",
                "cosmofs-copy-bitmap",
                r"cp->state == POOL_COPY_RESILVERING && cp->dev == bd[1] && !cp->whole && cp->since == whole_at",
                # Without a record nothing is raised to 13 and FAULTED is
                # not remembered across a remount.
                ("cosmofs-degraded",)),
    "resilver": (POOL,
                 """        if (!writable(&m->copy[c])) {""",
                 """        if (!readable(&m->copy[c])) {""",
                 "cosmofs-resilver",
                 r"raw_block(bd[1], CFS_DVA_BLK(dblk), y) && memcmp(y, during, CFS_BLOCK) == 0",
                 # The other resilvers end with copies that missed what
                 # was written while they ran.
                 ("cosmofs-resilver-resume", "cosmofs-spare", "cosmofs-attach")),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--case", choices=sorted(CASES), required=True)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    path, rule, without, test, check, also = CASES[a.case]
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "b3-copystate-probe" / f"{a.case}-{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"b3p-{a.arch[:3]}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            src = tree / path
            text = src.read_text()
            if text.count(rule) != 1:
                sys.exit(f"{path}: the rule's text is not there exactly once; the probe is stale")
            src.write_text(text.replace(rule, without))
        make = "gmake" if sys.platform == "darwin" else "make"
        out = tree / "out" / "dbg"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", f"OUT={out}",
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}"]
        with (work / "make.log").open("w") as log:
            rc = subprocess.run(args + ["test"], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                timeout=2400).returncode
        logs = list(out.glob("boot-test.log"))
        text = logs[0].read_text(errors="replace") if logs else ""
        (work / "boot-test.log").write_text(text)
        failed = re.findall(r"^SELFTEST: (\S+)\s+\.\.\. FAIL", text, re.M)
        mine = re.search(rf"^SELFTEST: {re.escape(test)}\s+\.\.\. (ok|FAIL: (.*))$", text, re.M)
        if a.old:
            at = mine is not None and mine.group(2) is not None and \
                re.match(rf"check failed: {re.escape(check)} at line \d+", mine.group(2)) is not None
            ok = at and test in failed and set(failed) <= {test, *also}
            seen = (f"{test} FAIL at `{check}`" + (f" (also {sorted(set(failed) - {test})})"
                                                     if set(failed) - {test} else "")
                    if ok else f"not that failure ({mine.group(0) if mine else 'no line'}; failed: {failed})")
        else:
            ok = rc == 0 and mine is not None and mine.group(1) == "ok" and not failed
            seen = (f"{test} ok, make test passed" if ok else
                    f"make test exited {rc}; {mine.group(0) if mine else 'no line'}; failed: {failed}")
        print(f"PROBE: {'PASS' if ok else 'FAIL'} case={a.case} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
