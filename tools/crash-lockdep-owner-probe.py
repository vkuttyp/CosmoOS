#!/usr/bin/env python3
"""Prove the CRASH_TEST=3 panic survives another CPU entering lockdep.

    crash-lockdep-owner-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

make test-display's check e boots a debug CRASH_TEST=3 kernel that leaves
lockdep's own raw lock held and panics, and requires that panic. The seam
faked the owner as the next CPU (this CPU's id + 2): a CPU that is
running, and that, entering lockdep before the panic stopped it, found its
own word and panicked first ("lockdep: graph raw lock re-entry"), which
main's CI saw at 621f7f78. The seam now makes that race certain -- a thread
pinned to the next CPU takes a tracked lock after the hold, and the crash
path waits until lockdep's raw lock reports a CPU waiting on the held word
-- and the fix names CPU CONFIG_MAX_CPUS, which no CPU is.

--old reverse-applies the fix commit's kernel/core/lockdep.c (the owner
word, and nothing else) in a throwaway worktree: the old word meets the
certain race and the next CPU's re-entry panic comes first. The seam and
the crash path are generic, so the probe boots the debug CRASH_TEST=3
image on either architecture (`make run`) and judges by the serial log.
The fix commit is found by its subject, or named with --fix (a squashed
merge keeps the change but not the subject).

Logs stay under out/crash-lockdep-owner-probe/<arch>-<mode>. Exit 0 when
the mode shows what it should (fixed: the crash test's own panic with a
CPU waiting on the word; old: the re-entry panic), else 1.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "crash test 3: lockdep's held word names a CPU no CPU is"
SOURCE = "kernel/core/lockdep.c"
REENTRY = "KERNEL PANIC: lockdep: graph raw lock re-entry on CPU"
OWN = "KERNEL PANIC: crash test: lockdep's lock is held by another CPU"
WAITING = "waiting on it)"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the commit that changed the owner word (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "crash-lockdep-owner-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"clp-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
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
        text = serial.read_text(errors="replace")
        if a.old:
            ok = REENTRY in text and OWN not in text
            seen = "the next CPU's re-entry panic came first" if ok else "no re-entry panic"
        else:
            own = [ln for ln in text.splitlines() if ln.startswith(OWN)]
            ok = bool(own) and WAITING in own[0] and REENTRY not in text
            seen = f"the crash test's own panic: {own[0][len('KERNEL PANIC: '):]}" if ok else "not the crash test's panic"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
