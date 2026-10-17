#!/usr/bin/env python3
"""Prove the CRASH_TEST=3 panic names a waiter on the held word only.

    crash3-waiter-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

make test-display's check e boots a debug CRASH_TEST=3 kernel that leaves
lockdep's own raw lock held and panics, and requires the panic to name
another CPU waiting on the held word. lockdep's raw lock recorded as that
waiter any CPU that waited on it, on any owner word, so a wait from before
the hold named the panicking CPU itself ("CPU 1 waiting on it", main's CI
at d4bf1e79). The seam now makes that stale wait certain -- this CPU waits
on lockdep's lock while the other CPU holds it, before the hold, and the
other CPU enters lockdep 10 ms after the hold -- and the fix records only a
wait on the held word.

--old reverse-applies the fix commit's kernel/core/lockdep.c in a
throwaway worktree: the stale wait is reported and names this CPU. The
probe boots the debug CRASH_TEST=3 image on either architecture
(`make run`) and judges by the serial log. The fix commit is found by its
subject, or named with --fix.

Logs stay under out/crash3-waiter-probe/<arch>-<mode>. Exit 0 when
the mode shows what it should (fixed: the crash test's own panic naming
another CPU as the waiter; old: naming this CPU), else 1.
"""
import argparse
import re
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "crash test 3: only a wait on the held word is the race"
SOURCE = "kernel/core/lockdep.c"
OWN = re.compile(r"^KERNEL PANIC: crash test: lockdep's lock is held by another CPU "
                 r"\(owner word (\d+), this CPU's (\d+), CPU (\d+) waiting on it\)", re.M)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the commit that changed the waiter record (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "crash3-waiter-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"c3w-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            sha = a.fix or subprocess.check_output(
                ["git", "-C", str(ROOT), "log", "-1", "--format=%H", "--fixed-strings", f"--grep={SUBJECT}", a.tree],
                text=True).strip()
            if not sha:
                sys.exit(f"no commit '{SUBJECT}' reachable from {a.tree}; name it with --fix")
            patch = subprocess.check_output(["git", "-C", str(ROOT), "show", "--format=", sha, "--", SOURCE])
            if not patch:
                sys.exit(f"commit {sha[:8]} does not change {SOURCE}")
            subprocess.run(["git", "-C", str(tree), "apply", "-R"], input=patch, check=True)
        make = "gmake" if sys.platform == "darwin" else "make"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", "BUILD=debug", "CRASH_TEST=3",
                f"OUT={tree / 'out' / 'paniclock'}",
                # BusyBox and the pinned sources are the same in either mode.
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}"]
        with (work / "build.log").open("w") as log:
            if subprocess.run(args + ["image"], stdout=log, stderr=subprocess.STDOUT).returncode != 0:
                sys.exit(f"the build failed ({work / 'build.log'})")
        # The panic ends the boot through the emulator exit; the bound is for
        # a boot that hangs instead.
        serial = work / "serial.log"
        with serial.open("w") as log:
            try:
                subprocess.run(args + ["run"], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                               timeout=600)
            except subprocess.TimeoutExpired:
                log.write("\nPROBE: the boot did not end in 600 s\n")
        text = serial.read_text(errors="replace").replace("\r", "")
        m = OWN.search(text)
        if not m:
            ok, seen = False, "not the crash test's panic"
        else:
            owner, this, waiter = (int(g) for g in m.groups())
            other = waiter not in (0, this, owner)
            ok = (waiter == this) if a.old else other
            seen = f"owner word {owner}, this CPU's {this}, waiter {waiter}" + (
                " (this CPU: the stale wait)" if waiter == this else " (another CPU)" if other else "")
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
