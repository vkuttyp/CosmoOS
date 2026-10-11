#!/usr/bin/env python3
"""Roadmap M3's acceptance test: BusyBox ash is the shell
(docs/userland/testing.md, "BusyBox").

    busybox_test.py --image cosmoos.img --workdir DIR [--timeout S]

One boot of the image. At the console, /bin/sh -- BusyBox ash -- runs
tests/busybox/ash.sh, the scripted test of every applet and of the
shell's features, which must end "BBTEST: PASS"; then
tests/busybox/suite.sh, BusyBox's own testsuite for the files decision 8
names. Every testsuite case must PASS, be SKIPPED or UNTESTED by
runtest itself (a feature the configuration leaves out), or be in
EXCLUDED below with its reason. Each file must have at least one PASS.
The serial log is kept in the work directory. Exit 0 PASS, 1 FAIL.
"""

import argparse
import os
import re
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from install_test import Boot, Fail, EXIT_SUCCESS  # noqa: E402

SUITE_FILES = ["cut", "sed", "grep", "tr", "sort", "uniq", "head", "tail", "expr", "seq", "basename", "dirname",
               "wc", "xargs", "tar"]

# (file, case name as runtest prints it) -> why it cannot pass here. Each
# is also listed in docs/userland/testing.md, "BusyBox".
_ON_LINUX = "fails the same way under Linux (arm64, this binary and testsuite): this build's behaviour, not the kernel's"
_HARDLINK = "makes a hard link (ln without -s); hard links are outside M3"
EXCLUDED = {
    ("sed", "sed embedded NUL"): _ON_LINUX,
    ("sed", "sed NUL in command"): _ON_LINUX,
    ("sed", "sed nonexistent label"): _ON_LINUX,
    ("tail", "tail: -c +N with largish N"): "needs dd, not an M3 applet",
    ("xargs", "xargs-works"): "needs md5sum, not an M3 applet",
    ("tar", "tar_with_link_with_size"): "needs bunzip2, not an M3 applet",
    ("tar", "tar_with_prefix_fields"): "needs bunzip2, not an M3 applet",
    ("tar", "tar Two zeroed blocks is a ('truncated') empty tarball"): "needs dd, not an M3 applet",
    ("tar", "tar Twenty zeroed blocks is an empty tarball"): "needs dd, not an M3 applet",
    ("tar", "tar extract tgz"): "needs dd, not an M3 applet",
    ("tar", "tar Symlink attack: create symlink and then write through it"): "needs uudecode, not an M3 applet",
    ("tar", "tar hardlinks and repeated files"): _HARDLINK,
    ("tar", "tar hardlinks mode"): _HARDLINK,
    ("tar", "tar symlinks mode"): _HARDLINK,
    ("tar", "tar --overwrite"): _HARDLINK,
    ("tar", "tar Symlinks and hardlinks coexist"): _HARDLINK,
}


def suite_results(out):
    """{file: [(verdict, case)]} from suite.sh's output, and the files
    whose run never ended."""
    per, cur, unended = {}, None, []
    for ln in out.splitlines():
        m = re.match(r"^BBSUITE: begin (\S+)$", ln)
        if m:
            cur = m.group(1)
            per[cur] = []
            continue
        m = re.match(r"^BBSUITE: end (\S+) (\d+)$", ln)
        if m:
            cur = None
            continue
        m = re.match(r"^(PASS|FAIL|SKIPPED|UNTESTED): (.*)$", ln)
        if m and cur is not None:
            per[cur].append((m.group(1), m.group(2).strip()))
    for f in SUITE_FILES:
        if f not in per:
            unended.append(f)
    return per, unended


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--timeout", type=float, default=900.0)
    ap.add_argument("--verbose", action="store_true", help="runtest -v: each failing case's diff in the log")
    args = ap.parse_args()
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    image = os.path.join(args.workdir, "cosmoos.img")
    shutil.copyfile(args.image, image)
    t0 = time.monotonic()
    lines = []
    b = Boot("busybox", image, args.workdir, {}, args.timeout)
    try:
        b.wait_prompt("the first prompt")
        out = b.run("/bin/sh /boot/tests/busybox/ash.sh")
        script = [ln for ln in out.splitlines() if ln.startswith("BBTEST: ")]
        failed = [ln for ln in script if ln.startswith("BBTEST: FAIL")]
        m = re.search(r"^BBTEST: (\d+) checks, (\d+) failed$", out, re.M)
        if not m or "BBTEST: PASS" not in script or failed:
            raise Fail("scripted test: " + ("; ".join(failed) if failed else "no verdict line"))
        lines.append(f"scripted test: PASS, {m.group(1)} checks")
        t1 = time.monotonic()
        out = b.run("/bin/sh -c 'BBSUITE_VERBOSE=1 /bin/sh /boot/tests/busybox/suite.sh'" if args.verbose
                    else "/bin/sh /boot/tests/busybox/suite.sh")
        if "BBSUITE: done" not in out:
            raise Fail("testsuite: suite.sh did not finish")
        per, unended = suite_results(out)
        if unended:
            raise Fail(f"testsuite: no run of {', '.join(unended)}")
        bad = []
        for f in SUITE_FILES:
            counts = {"PASS": 0, "FAIL": 0, "SKIPPED": 0, "UNTESTED": 0, "excluded": 0}
            for verdict, case in per[f]:
                if verdict == "FAIL" and (f, case) in EXCLUDED:
                    counts["excluded"] += 1
                    continue
                counts[verdict] += 1
                if verdict == "FAIL":
                    bad.append(f"{f}: {case}")
            if counts["PASS"] == 0:
                bad.append(f"{f}: no case passed")
            lines.append(f"testsuite {f}: {counts['PASS']} pass, {counts['excluded']} excluded, "
                         f"{counts['SKIPPED']} skipped, {counts['UNTESTED']} untested, {counts['FAIL']} fail")
        lines.append(f"testsuite: {time.monotonic() - t1:.1f}s")
        if bad:
            raise Fail("testsuite failures: " + "; ".join(bad))
        rc = b.finish("exit 0")
        if rc != EXIT_SUCCESS:
            raise Fail(f"QEMU exit code {rc}, expected {EXIT_SUCCESS}")
    except Fail as e:
        b.kill()
        for ln in lines:
            print(f"test-busybox: {ln}")
        print(f"test-busybox: FAIL: {e} (log in {args.workdir})")
        return 1
    except Exception:
        b.kill()
        raise
    for ln in lines:
        print(f"test-busybox: {ln}")
    print(f"test-busybox: PASS in {time.monotonic() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
