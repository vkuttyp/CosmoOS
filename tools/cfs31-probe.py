#!/usr/bin/env python3
"""Prove the member table's seal is checked at mount (CFS-31).

    cfs31-probe.py [--old] [--arch x86_64|aarch64] [--tree REV]

cfs_members_load reads the member table raw -- the buffer cache checks an
address against the very table it is loading -- and checked its magic,
kind and address only. Its CRC32C was never compared, so a table damaged
in place was used: found by roadmap 1.2 B5, when a member table forged
on an encrypted pool mounted.

The self-test cosmofs-encrypted-root damages one byte of a plain pool's
member table (member 0's advisory free count, the CRC left as it was) and
requires the check's mount to refuse it -- fail, or fall back to the
older root -- and forges an encrypted pool's with its CRC32C recomputed.

  fixed  a debug `make test`: cosmofs-encrypted-root passes.
  --old  in a throwaway worktree, cosmofs_member.cee's check put back to
         what it was before the fix (magic, kind and address; no seal):
         the plain pool's damaged table is ACCEPTED, and
         cosmofs-encrypted-root is the only self-test that fails. (The
         fix was made in cosmofs_member.c; the file is CeeSafe now,
         roadmap 2.0 SA7, so the old check is written here in CeeSafe.)

Logs stay under out/cfs31-probe/<arch>-<mode>. Exit 0 when the mode shows
what it should, else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MEMBER = "kernel-services/filesystem/cosmofs/cosmofs_member.cee"
# The check as the fix made it, and as it was before: the header's magic, kind and address, and no seal.
RULE = """    if (rc == 0 && !cfs_mhdr_ok(borrow fsOf(fs).mac as *cfs_mac, block as *void, at, CFS_KIND_MEMBERS)) {
        rc = -EIO;
    }"""
OLD = """    let header = Slice.fromRaw(block as *u8, CFS_BLOCK);
    if (rc == 0 && (ByteOrder.readU32LE(header, 0) != 0x4d534643 || ByteOrder.readU32LE(header, 4) != CFS_KIND_MEMBERS ||
        ByteOrder.readU64LE(header, 16) != at)) {
        rc = -EIO;
    }"""
ACCEPTED = re.compile(r"cosmofs-encrypted-root: a plain pool's member table damaged in place: ACCEPTED")
REFUSED = re.compile(r"cosmofs-encrypted-root: a plain pool's member table damaged in place: refused")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "cfs31-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"c31-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            src = tree / MEMBER
            text = src.read_text()
            if text.count(RULE) != 1:
                sys.exit(f"{MEMBER}: the check's text is not there exactly once; the probe is stale")
            src.write_text(text.replace(RULE, OLD))
        make = "gmake" if sys.platform == "darwin" else "make"
        out = tree / "out" / "o"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", f"OUT={out}",
                # BusyBox and the pinned sources are the same in either mode.
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}", "test"]
        with (work / "make.log").open("w") as log:
            rc = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                timeout=3600).returncode
        logs = list(out.glob("boot-test.log"))
        text = logs[0].read_text(errors="replace") if logs else ""
        (work / "boot-test.log").write_text(text)
        failed = re.findall(r"^SELFTEST: (\S+)\s+\.\.\. FAIL", text, re.M)
        if a.old:
            ok = ACCEPTED.search(text) is not None and failed == ["cosmofs-encrypted-root"]
            seen = ("a plain pool's member table damaged in place was ACCEPTED (cosmofs-encrypted-root FAIL)"
                    if ok else f"no such failure alone (failed: {failed})")
        else:
            ok = rc == 0 and REFUSED.search(text) is not None and not failed
            seen = ("the damaged member table refused (make test passes)" if ok else
                    f"make test exited {rc}; failed: {failed}")
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
