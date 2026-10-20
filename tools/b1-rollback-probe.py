#!/usr/bin/env python3
"""Show why a rollback's record and its safety snapshot are one commit (CF19).

    b1-rollback-probe.py [--old] [--arch x86_64|aarch64] [--tree REV]

`rollback NAME` takes a safety snapshot of the live tree and writes a
pending record naming it and the target, and both reach the disk in one
transaction (cfs_rollback_request, cosmofs_rollback.c). The self-test
cosmofs-rollback-atomic stops the machine at every commit boundary the
request crosses (cosmofs_test_commit_budget), mounts the filesystem again
and requires either neither or both -- and a checker that finds no
rollback record naming a missing snapshot.

--old splits the rule in a throwaway worktree: the record is committed on
its own first, and the safety snapshot in a commit after it. A stop
between the two then leaves a durable record whose safety snapshot does
not exist: the tree the rollback would replace has no snapshot keeping it.
The mount refuses that record only because it also checks that both
snapshots exist (cfs_rollback_at_mount); without that second check the
switch would release the old tree's blocks and nothing could bring it
back. The self-test fails at its check that the checker names no
rollback record. The check runs in a debug `make test` on either
architecture.

Logs stay under out/b1-rollback-probe/<arch>-<mode>. Exit 0 when the mode
shows what it should (fixed: cosmofs-rollback-atomic ok; old: it fails at
the rollback-record check), else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = "kernel-services/filesystem/cosmofs/cosmofs_rollback.c"
OK = re.compile(r"^SELFTEST: cosmofs-rollback-atomic\s+\.\.\. ok ", re.M)
FAILED = re.compile(r"^SELFTEST: cosmofs-rollback-atomic\s+\.\.\. FAIL: check failed: "
                    r"rep\.rollback_record\.count == 0 && rep\.hold\.count == 0 at line \d+", re.M)

# The rule as built: the safety snapshot joins the open transaction, the
# record joins it, one commit.
ONE_COMMIT = """    struct cfs_snapshot s;
    rc = cfs_snapshot_create_ex(fs, name, true, false, r.safety_uuid, &s);
    if (rc)
        return rc;
    /* The raise to 12 can have happened just now, so the target is read
     * again for its uuid. */
    if (!cfs_snapshot_find(fs, target, &t)) {
        cfs_fail(fs, -EIO);
        return -EIO;
    }
    memcpy(r.target_uuid, t.uuid, 16);
    r.target_id = t.id;
    r.safety_id = s.id;
    r.generation = fs->sb.generation;
    rc = rb_write(fs, &r);
    if (rc) {
        cfs_fail(fs, rc);   /* the safety entry is in the transaction: not without its record */
        return rc;
    }
    rc = rb_commit(fs);"""

# The rule split: the record committed on its own, then the snapshot.
TWO_COMMITS = """    memcpy(r.target_uuid, t.uuid, 16);
    r.target_id = t.id;
    r.generation = fs->sb.generation;
    rc = cfs_commit(fs);
    if (rc == 0)
        rc = rb_write(fs, &r);
    if (rc == 0)
        rc = rb_commit(fs);                 /* probe --old: the record alone */
    struct cfs_snapshot s;
    if (rc == 0)
        rc = cfs_snapshot_create_ex(fs, name, true, true, r.safety_uuid, &s);   /* then the snapshot */"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "b1-rollback-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"b1r-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            src = tree / SOURCE
            text = src.read_text()
            if text.count(ONE_COMMIT) != 1:
                sys.exit(f"{SOURCE} at {a.tree} does not hold the one-commit request this probe splits")
            src.write_text(text.replace(ONE_COMMIT, TWO_COMMITS))
            if TWO_COMMITS not in src.read_text():
                sys.exit("the split did not apply")
        make = "gmake" if sys.platform == "darwin" else "make"
        out = tree / "out" / "dbg"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", f"OUT={out}",
                # BusyBox and the pinned sources are the same in either mode.
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}"]
        with (work / "make.log").open("w") as log:
            subprocess.run(args + ["test"], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                           timeout=2400)
        logs = list(out.glob("boot-test.log"))
        text = logs[0].read_text(errors="replace") if logs else ""
        (work / "boot-test.log").write_text(text)
        stops = re.findall(r"cosmofs-rollback-atomic: stopped after (\d+) commit\(s\): request (-?\d+), pool flags "
                           r"(\d+), safety flags (0x[0-9a-f]+)", text)
        if a.old:
            ok = FAILED.search(text) is not None
            seen = ("a stop between the two commits left a record naming a safety snapshot that does not exist "
                    "(cosmofs-rollback-atomic FAIL)" if ok else "no such failure")
        else:
            ok = OK.search(text) is not None
            seen = "every stop left neither or both (cosmofs-rollback-atomic ok)" if ok else "it did not pass"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen}; stops {stops} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
