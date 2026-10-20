#!/usr/bin/env python3
"""Show why a shell's foreground job takes the terminal as it is spawned.

    spawn-foreground-probe.py [--old] [--arch x86_64|aarch64] [--tree REV]

cosmo-sh spawned a job and then made its group the terminal's foreground
with tcsetpgrp. A job that asked at its start whether it was in the
foreground could run first: an interactive ash started from cosmo-sh found
the shell's group there and stopped itself with SIGTTIN, which turned
main's busybox test red (docs/testing/flakes.md, "ash history: the nested
shell stopped"). COSMO_SPAWN_FOREGROUND hands the terminal to the child's
group before its first instruction (process.c, tty_hand_foreground).

The self-test spawn-foreground spawns a job with the flag and never calls
tcsetpgrp itself -- the race lost every time -- and the job checks
tcgetpgrp() == getpgrp() as its first act; a control without the flag must
find itself in the background.

--old removes the hand-over in a throwaway worktree (the flag is still
accepted and does nothing, as the kernel did before it): the job then finds
the spawner's group in the foreground, and spawn-foreground fails at its
check 7. The check runs in a debug `make test` on either architecture.

Logs stay under out/spawn-foreground-probe/<arch>-<mode>. Exit 0 when the
mode shows what it should (fixed: spawn-foreground ok; old: it fails with
check 7), else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = "kernel/process/process.c"
OK = re.compile(r"^SELFTEST: spawn-foreground\s+\.\.\. ok ", re.M)
FAILED = re.compile(r"selftest: spawn-foreground: the probe failed check 7$", re.M)

HAND_OVER = """            (void)tty_hand_foreground(tt, p->sid, attr->parent->pgid, p->pgid);
"""
NO_HAND_OVER = """            (void)tt;   /* probe --old: the job is spawned and the terminal left as it was */
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "spawn-foreground-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"sfg-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            src = tree / SOURCE
            text = src.read_text()
            if text.count(HAND_OVER) != 1:
                sys.exit(f"{SOURCE} at {a.tree} does not hold the hand-over this probe removes")
            src.write_text(text.replace(HAND_OVER, NO_HAND_OVER))
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
            seen = ("the job found the spawner's group in the foreground (spawn-foreground FAIL, check 7)" if ok
                    else "no such failure")
        else:
            ok = OK.search(text) is not None
            seen = "the job had the terminal at its first instruction (spawn-foreground ok)" if ok else "it did not pass"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
