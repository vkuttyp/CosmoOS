#!/usr/bin/env python3
"""Roadmap M3's acceptance test: BusyBox ash is the shell
(docs/userland/testing.md, "BusyBox").

    busybox_test.py --image cosmoos.img --workdir DIR [--timeout S]

One boot of the image. At the console, /bin/sh -- BusyBox ash -- runs
tests/busybox/ash.sh, the scripted test of every applet and of the
shell's features, which must end "BBTEST: PASS"; then an interactive ash
whose command must be in /root/.ash_history and recalled by the next; then
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


# The dynamic pass's environment: the dynamic applets first.
DYN_ENV = "env PATH=/usr/bin:/bin:/sbin:/usr/sbin"


def scripted(b, cmd, what):
    """Run ash.sh by `cmd`; its verdict line, or Fail."""
    out = b.run(cmd)
    script = [ln for ln in out.splitlines() if ln.startswith("BBTEST: ")]
    failed = [ln for ln in script if ln.startswith("BBTEST: FAIL")]
    m = re.search(r"^BBTEST: (\d+) checks, (\d+) failed$", out, re.M)
    if not m or "BBTEST: PASS" not in script or failed:
        raise Fail(f"{what}: " + ("; ".join(failed) if failed else "no verdict line"))
    return f"{what}: PASS, {m.group(1)} checks"


def suite(b, lines, cmd, what):
    """Run suite.sh by `cmd` and judge it against EXCLUDED."""
    t1 = time.monotonic()
    out = b.run(cmd)
    if "BBSUITE: done" not in out:
        raise Fail(f"{what}: suite.sh did not finish")
    per, unended = suite_results(out)
    if unended:
        raise Fail(f"{what}: no run of {', '.join(unended)}")
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
        lines.append(f"{what} {f}: {counts['PASS']} pass, {counts['excluded']} excluded, "
                     f"{counts['SKIPPED']} skipped, {counts['UNTESTED']} untested, {counts['FAIL']} fail")
    lines.append(f"{what}: {time.monotonic() - t1:.1f}s")
    if bad:
        raise Fail(f"{what} failures: " + "; ".join(bad))


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
    b = None
    try:
        # Inside the try: a second firmware stall is a Fail from Boot().
        b = Boot("busybox", image, args.workdir, {}, args.timeout)
        b.wait_prompt("the first prompt")
        lines.append(scripted(b, "/bin/sh /boot/tests/busybox/ash.sh", "scripted test"))
        # ash's history (shell completeness S2): an interactive ash on the
        # console keeps each command in $HOME/.ash_history (HOME=/root), and
        # a new ash reads it back.
        marker = f"echo bbhist-{os.getpid()}"
        b.run("sh")
        b.run(marker)
        b.run("exit")
        out = b.run("cat /root/.ash_history")
        if not re.search(rf"^{re.escape(marker)}$", out, re.M):
            raise Fail(f"ash history: /root/.ash_history lacks {marker!r}: {out[-400:]!r}")
        b.run("sh")
        out = b.run("history")
        b.run("exit")
        if not re.search(rf"^\s*\d+\s+{re.escape(marker)}$", out, re.M):
            raise Fail(f"ash history: a new ash does not recall {marker!r}: {out[-400:]!r}")
        lines.append(f"ash history: {marker!r} in /root/.ash_history, recalled by a new ash")
        verbose = "BBSUITE_VERBOSE=1 " if args.verbose else ""
        suite(b, lines, f"/bin/sh -c '{verbose}/bin/sh /boot/tests/busybox/suite.sh'", "testsuite")

        # Roadmap 1.1 P4: the same script and testsuite under the
        # dynamically linked BusyBox, /bin/busybox.dyn (musl's dynamic
        # linker its PT_INTERP), its applets first in PATH.
        out = b.run(f"{DYN_ENV} /bin/busybox.dyn sh -c 'readlink /proc/$$/exe; command -v ls; echo dyn-ok'")
        for want, what in ((r"^/bin/busybox\.dyn$", "the dynamic shell's executable"),
                           (r"^/usr/bin/ls$", "the dynamic applets first in PATH"), (r"^dyn-ok$", "the dynamic shell")):
            if not re.search(want, out, re.M):
                raise Fail(f"dynamic BusyBox: {what}: no line matching {want!r} in {out[-400:]!r}")
        lines.append(scripted(b, f"{DYN_ENV} BBTEST_BIN=/usr/bin BBTEST_EXE=/bin/busybox.dyn /bin/busybox.dyn sh "
                                 "/boot/tests/busybox/ash.sh", "dynamic scripted test"))
        suite(b, lines, f"{DYN_ENV} {verbose}BBSUITE_BINARY=/bin/busybox.dyn /bin/busybox.dyn sh "
                        "/boot/tests/busybox/suite.sh", "dynamic testsuite")
        lines.append("dynamic BusyBox: /bin/busybox.dyn ran ash.sh and the testsuite with /usr/bin first in PATH")
        # The dlopen test program and the auxv/RELRO/W^X/TLS checker, from
        # the dynamic shell (tests/dyn/).
        out = b.run(f"{DYN_ENV} /bin/busybox.dyn sh -c '/boot/tests/dyn/dynhello && /boot/tests/dyn/dltest; "
                    "echo dyn-tests=$?'")
        for want, what in ((r"^dynhello: ok$", "dynhello"), (r"^dltest: ok$", "dltest (dlopen)"),
                           (r"^dyn-tests=0$", "the dynamic test programs' exit")):
            if not re.search(want, out, re.M):
                raise Fail(f"dynamic programs: {what}: no line matching {want!r} in {out[-600:]!r}")
        lines.append("dynamic programs: dynhello and dltest (dlopen) passed under the dynamic shell")
        rc = b.finish("exit 0")
        if rc != EXIT_SUCCESS:
            raise Fail(f"QEMU exit code {rc}, expected {EXIT_SUCCESS}")
    except Fail as e:
        if b is not None:
            b.kill()
        for ln in lines:
            print(f"test-busybox: {ln}")
        print(f"test-busybox: FAIL: {e} (log in {args.workdir})")
        return 1
    except Exception:
        if b is not None:
            b.kill()
        raise
    for ln in lines:
        print(f"test-busybox: {ln}")
    print(f"test-busybox: PASS in {time.monotonic() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
