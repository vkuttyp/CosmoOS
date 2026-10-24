#!/usr/bin/env python3
"""Prove a stale service pid file no longer stops the service (USR-8).

    usr8-probe.py [--old] [--check selftest|install] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

`svc` read /run/svc/<name>.pid as running whenever kill(pid, 0) could
signal the pid, and on an installed system /run was on the root disk, so
the file outlived its service: on the Parallels VM (2026-10-23)
dropbear.pid held 13 from the day before, pid 13 was syslogd at every
boot, and `svc: dropbear is already running` kept ssh refused through
several boots.

Two checks, each against the fix commit:

  selftest  init's self-test svc_stale_selftest writes pid files naming
            the test process itself (live, and not a supervisor) and
            requires the service to start. --old reverse-applies the fix
            commit's svc.c in a throwaway worktree: `svc: stale is already
            running`, and process-user is the only self-test that fails.
            A debug `make test`.
  install   test-install's reboot stage leaves pid files naming pid 1 on
            the root's own /run and the persist stage requires every
            service to start. --old reverse-applies the commit's svc.c and
            init.c (the /run ramfs): the persist boot fails with `a stale
            pid file stopped a service from starting`. `make BUILD=release
            test-install`.

Logs stay under out/usr8-probe/<check>-<arch>-<mode>. Exit 0 when the
mode shows what it should, else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "svc: a pid file is this boot's and names its own process (USR-8)"
SOURCES = {"selftest": ["userland/system/svc.c"], "install": ["userland/system/svc.c", "userland/init/init.c"]}
STALE = re.compile(r"^svc: stale is already running$", re.M)
INSTALL_FAIL = re.compile(r"a stale pid file stopped a service from starting \(USR-8\)")
INSTALL_OK = re.compile(r"^test-install: persist: PASS .*every service started past stale pid files", re.M)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--check", choices=("selftest", "install"), default="selftest")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the fix commit (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "usr8-probe" / f"{a.check}-{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"u8-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
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
            patch = subprocess.check_output(["git", "-C", str(ROOT), "show", "--format=", sha, "--"] +
                                            SOURCES[a.check])
            if not patch:
                sys.exit(f"commit {sha[:8]} does not change {SOURCES[a.check]}")
            subprocess.run(["git", "-C", str(tree), "apply", "-R"], input=patch, check=True)
        make = "gmake" if sys.platform == "darwin" else "make"
        out = tree / "out" / "o"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", f"OUT={out}",
                # BusyBox and the pinned sources are the same in either mode.
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}"]
        args += ["test"] if a.check == "selftest" else ["BUILD=release", "test-install"]
        with (work / "make.log").open("w") as log:
            rc = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                timeout=3600).returncode
        made = (work / "make.log").read_text(errors="replace")
        if a.check == "selftest":
            logs = list(out.glob("boot-test.log"))
            text = logs[0].read_text(errors="replace") if logs else ""
            (work / "boot-test.log").write_text(text)
            failed = re.findall(r"^SELFTEST: (\S+)\s+\.\.\. FAIL", text, re.M)
            if a.old:
                ok = STALE.search(text) is not None and failed == ["process-user"]
                seen = ("`svc: stale is already running` for a pid file naming the test (process-user FAIL)"
                        if ok else f"no such failure alone (failed: {failed})")
            else:
                ok = rc == 0 and STALE.search(text) is None and not failed
                seen = ("the stale pid files passed over, the service started (make test passes)" if ok else
                        f"make test exited {rc}; failed: {failed}")
        else:
            if a.old:
                ok = rc != 0 and INSTALL_FAIL.search(made) is not None
                seen = ("the boot after stale pid files did not start a service (persist FAIL)" if ok else
                        f"make exited {rc} without that failure")
            else:
                ok = rc == 0 and INSTALL_OK.search(made) is not None
                seen = ("every service started past stale pid files (test-install passes)" if ok else
                        f"make exited {rc}")
        print(f"PROBE: {'PASS' if ok else 'FAIL'} check={a.check} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
