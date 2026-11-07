#!/usr/bin/env python3
"""Prove a failed mapping leaves the old block under its own checksum (CF16).

    cosmofs-extent-fail-probe.py [--old] [--arch x86_64|aarch64] [--tree REV]

data_write_block stored a block's checksum before set_extent mapped the
block, and record_write a record's before its set_extent. When the mapping
failed (-ENOMEM, -EFBIG), the old data stayed mapped under the new
checksum and every later read of it failed: "no copy matches its
checksum" (inventory section 17, after shell completeness S7, whose
record-fallback defect was one way to get there).

The self-test cosmofs-extent-fail fails one set_extent of a write-back
through the FI_COSMOFS_EXTENT_NOMEM/FBIG seam and reads every block the
open inode maps (cosmofs_test_verify_file). With the fix every block
verifies; without it the first case, plain blocks over plain blocks,
fails `bad == 0`. The fix's sources are CeeSafe now (roadmap 2.0 SA7),
so --old takes its parts out of the ports by their text, in a throwaway
worktree, keeping the seam and the self-test: the saved entries put back
nowhere (cosmofs_data.cee's csumRestore, the CF16 half the verdict
rests on) and the check's two lines (cosmofs_check.cee). The check runs
in a debug `make test` on either architecture.

Logs stay under out/cosmofs-extent-fail-probe/<arch>-<mode>. Exit 0 when
the mode shows what it should (fixed: cosmofs-extent-fail ok; old: it
fails at `bad == 0`), else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
# The fix's CF16 half in cosmofs_data.cee: a saved checksum entry is put back when the mapping fails. --old keeps the
# save and puts nothing back, which is what the code before the fix did.
DATA = "kernel-services/filesystem/cosmofs/cosmofs_data.cee"
DATA_RULE = ("unsafe fn csumRestore(fs: *mut cfs, inp: *mut cfs_inode, lblk: u64, s: *mut u64) {\n"
             "    if (s.read() == 0) {\n")
DATA_OLD = ("unsafe fn csumRestore(fs: *mut cfs, inp: *mut cfs_inode, lblk: u64, s: *mut u64) {\n"
            "    if (s.read() == 0 || s.read() != 0) { // --old: nothing is put back\n")
# The fix's two lines in cosmofs_check.cee.
CHECK = "kernel-services/filesystem/cosmofs/cosmofs_check.cee"
CHECK_RULE = (" else {\n"
              "                    cfs_fail(fs, rc); // the frees are queued and the slot still names them (CF17)\n"
              "                }\n")
CHECK_OLD = "\n"
OK = re.compile(r"^SELFTEST: cosmofs-extent-fail\s+\.\.\. ok ", re.M)
FAILED = re.compile(r"^SELFTEST: cosmofs-extent-fail\s+\.\.\. FAIL: check failed: bad == 0 at line \d+", re.M)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "cosmofs-extent-fail-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"cef-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            src = tree / DATA
            body = src.read_text()
            if body.count(DATA_RULE) != 1:
                sys.exit(f"{DATA}: the fixed text is not there exactly once; the probe is stale")
            src.write_text(body.replace(DATA_RULE, DATA_OLD))
            src = tree / CHECK
            body = src.read_text()
            if body.count(CHECK_RULE) != 1:
                sys.exit(f"{CHECK}: the fixed text is not there exactly once; the probe is stale")
            src.write_text(body.replace(CHECK_RULE, CHECK_OLD))
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
        if a.old:
            ok = FAILED.search(text) is not None
            seen = "a block stayed mapped under a checksum not its own (cosmofs-extent-fail FAIL)" if ok else "no such failure"
        else:
            ok = OK.search(text) is not None
            seen = "every mapped block verified (cosmofs-extent-fail ok)" if ok else "cosmofs-extent-fail did not pass"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
