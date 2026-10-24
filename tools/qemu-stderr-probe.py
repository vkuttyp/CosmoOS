#!/usr/bin/env python3
"""Prove QEMU's stderr stays out of the guest's lines (TEST-20).

    qemu-stderr-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

The boot harnesses started QEMU with its stderr in the serial pipe, so a
message QEMU printed while the guest was in the middle of a line split
that line in the text the tests match: main's test-mirror (run
40077407190) missed `pool: ... MISSING` that way, with QEMU's `wrong
value for queue_enable 0` from a hot unplug inside it.

The probe makes that race certain. QEMU_WRAP runs QEMU under a splitter
(written to the probe's output directory) that passes the guest's serial
output through and, inside the first `init: switched to the disk root`
line, writes a QEMU-style message to its stderr. Then a release `make
test-install`:

  fixed  test-install passes, and the message is in a boot's -qemu.log.
  --old  the fix commit's tests/boot changes reverse-applied in a
         throwaway worktree: test-install fails on that line (`no line
         matching '^init: switched to the disk root ...`).

Logs stay under out/qemu-stderr-probe/<arch>-<mode>. Exit 0 when the mode
shows what it should, else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "tests/boot: QEMU's stderr in a file of its own, not inside guest lines (TEST-20)"
SOURCES = ["tests/boot/install_test.py", "tests/boot/platform_test.py", "tests/boot/run_boot_test.py"]
MESSAGE = "qemu-system: qemu-stderr-probe: a message inside a guest line"

SPLITTER = r'''#!/usr/bin/env python3
"""QEMU under the probe: its serial output passed through, and a message
on stderr written inside the first line holding KEY."""
import os
import subprocess
import sys
import time

KEY = b"init: switched to the disk root"
p = subprocess.Popen(sys.argv[1:], stdout=subprocess.PIPE)
out = sys.stdout.buffer
pending = b""
done = False
while True:
    chunk = os.read(p.stdout.fileno(), 4096)
    if not chunk:
        break
    if done:
        out.write(chunk)
        out.flush()
        continue
    pending += chunk
    i = pending.find(KEY)
    if i < 0:
        # Hold back only a tail that could begin KEY: the rest, a
        # prompt among it, goes out at once.
        keep = next((k for k in range(min(len(KEY) - 1, len(pending)), 0, -1)
                     if KEY.startswith(pending[-k:])), 0)
        out.write(pending[:len(pending) - keep])
        pending = pending[len(pending) - keep:]
        out.flush()
        continue
    cut = i + len(KEY) // 2
    out.write(pending[:cut])
    out.flush()
    time.sleep(0.2)
    os.write(2, b"MESSAGE\n")
    time.sleep(0.2)
    out.write(pending[cut:])
    out.flush()
    pending = b""
    done = True
out.write(pending)
out.flush()
sys.exit(p.wait())
'''.replace("MESSAGE", MESSAGE)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the fix commit (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "qemu-stderr-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    splitter = work / "split.py"
    splitter.write_text(SPLITTER)
    splitter.chmod(0o755)
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"qse-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
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
        out = tree / "out" / "o"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", "BUILD=release", f"OUT={out}",
                f"QEMU_WRAP={sys.executable} {splitter}",
                # BusyBox and the pinned sources are the same in either mode.
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}",
                "test-install"]
        with (work / "make.log").open("w") as log:
            rc = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                timeout=3600, start_new_session=True).returncode
        text = (work / "make.log").read_text(errors="replace")
        verdict = re.search(r"^test-install: (PASS|FAIL).*$", text, re.M)
        qemu_logs = "".join(p.read_text(errors="replace") for p in (out / "test-install").glob("*-qemu.log"))
        if a.old:
            ok = (rc != 0 and verdict is not None and verdict.group(1) == "FAIL"
                  and "init: switched to the disk root" in verdict.group(0))
            seen = (f"the split line missed: {verdict.group(0)}" if ok else
                    f"no such failure (make exit {rc}; {verdict.group(0) if verdict else 'no verdict'})")
        else:
            ok = rc == 0 and verdict is not None and verdict.group(1) == "PASS" and MESSAGE in qemu_logs
            seen = ("test-install passes; QEMU's message is in a -qemu.log" if ok else
                    f"make exit {rc}; {verdict.group(0) if verdict else 'no verdict'}; "
                    f"message in -qemu.log: {MESSAGE in qemu_logs}")
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
