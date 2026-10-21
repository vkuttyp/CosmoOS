#!/usr/bin/env python3
"""Prove a console write holds the console lock a line at a time.

    console-lines-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

console_write took g_console_lock with interrupts off once for the whole
write and passed it to every sink, fbcon among them (a scroll repaints
the screen, 11 ms under QEMU's TCG). `dmesg` writes the kernel log in one
32 KiB write, which arrives as one console_write: CI run 39568825512
(PR #429, entropy boots x86_64) had cpu 2 without a tick for 1223 ms in
`dmesg`, interrupts coming back in arch_irq_restore_hw, and cpu 0's TLB
shootdown, waiting a second for cpu 2, panicked.

The self-test console-lines writes 24 lines (about 1.1 KiB, more than
CONSOLE_HOLD_MAX) in one console_write and asks how many bytes the
largest hold passed to the sinks. With the fix that is one line; without
it, the whole write, and the check `most > 0 && most <= CONSOLE_HOLD_MAX`
fails. --old reverse-applies the fix commit (its console.c, the whole
commit's change to it and nothing else) in a throwaway worktree, keeping
the self-test and the count. The check runs in a debug `make test` on
either architecture.

Logs stay under out/console-lines-probe/<arch>-<mode>. Exit 0 when the
mode shows what it should (fixed: `make test` passes; old: console-lines
fails at that check with the whole write in one hold, and no other
self-test fails), else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "console: a write reaches the sinks a line per hold"
SOURCES = ["kernel/core/console.c"]
OK = re.compile(r"^SELFTEST: console-lines\s+\.\.\. ok ", re.M)
FAILED = re.compile(r"^SELFTEST: console-lines\s+\.\.\. FAIL: fbcon: most > 0 && most <= CONSOLE_HOLD_MAX \(", re.M)
COUNTS = re.compile(r"selftest: console-lines: (\d+) bytes in \d+ lines in (\d+) us; the most one hold carried was "
                    r"(\d+) bytes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the fix commit (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "console-lines-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"cl-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
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
            patch = subprocess.check_output(["git", "-C", str(ROOT), "show", "--format=", sha, "--"] + SOURCES)
            if not patch:
                sys.exit(f"commit {sha[:8]} does not change {SOURCES}")
            subprocess.run(["git", "-C", str(tree), "apply", "-R"], input=patch, check=True)
        make = "gmake" if sys.platform == "darwin" else "make"
        out = tree / "out" / "dbg"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", f"OUT={out}",
                # BusyBox and the pinned sources are the same in either mode.
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}"]
        with (work / "make.log").open("w") as log:
            rc = subprocess.run(args + ["test"], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                timeout=2400).returncode
        logs = list(out.glob("boot-test.log"))
        text = logs[0].read_text(errors="replace") if logs else ""
        (work / "boot-test.log").write_text(text)
        # The old mode's boot fails, so `make test` exits non-zero in it: what
        # tells its expected failure from any other is that this self-test is
        # the only one that fails. The fixed mode needs the whole boot to pass.
        failed = re.findall(r"^SELFTEST: (\S+)\s+\.\.\. FAIL", text, re.M)
        c = COUNTS.search(text)
        if a.old:
            ok = (FAILED.search(text) is not None and failed == ["console-lines"] and c is not None and
                  int(c.group(3)) == int(c.group(1)))
            seen = (f"the whole {c.group(1)}-byte write in one hold, {c.group(2)} us (console-lines FAIL)"
                    if ok else f"no such failure alone (failed: {failed})")
        else:
            ok = rc == 0 and OK.search(text) is not None and not failed and c is not None
            seen = (f"{c.group(1)} bytes, the largest hold {c.group(3)} bytes, {c.group(2)} us (console-lines ok)"
                    if ok else f"make test exited {rc}; console-lines {'ok' if OK.search(text) else 'not ok'}; "
                    f"failed: {failed}")
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
