#!/usr/bin/env python3
"""Check a diagnostic repair, or show the defect in the baseline's own tree (--old).

The analyzer itself is the deterministic check. Only the named function's
diagnostic is the verdict; a compiler failure is never a successful proof.
"""

import argparse
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BASELINE = "55b1825ebc28c7f5cdb948477019808dc8ed5517"
CASES = {
    "smp-affinity": ("kernel/scheduler/smptest.c", "sched_balance_affinity_pinned",
                     "core.UndefinedBinaryOperatorResult", 1),
    "cosmofs-corrupt": ("kernel-services/filesystem/cosmofs/cosmofs_core.c", "cosmofs_test_corrupt", "core.", 3),
    "cosmofs-freelog": ("kernel-services/filesystem/cosmofs/cosmofs_core.c", "freelog_fill", "deadcode.DeadStores", 1),
    "cosmofs-load-root": ("kernel-services/filesystem/cosmofs/cosmofs_core.c", "load_root", "deadcode.DeadStores", 1),
    "cosmofs-badmap": ("kernel-services/filesystem/cosmofs/cosmofstest.c", "selftest_cosmofs_badmap", "unix.Malloc", 1),
    "cosmofs-mirror": ("kernel-services/filesystem/cosmofs/cosmofstest.c", "selftest_cosmofs_mirror", "unix.Malloc", 1),
    "cosmofs-compress": ("kernel-services/filesystem/cosmofs/cosmofstest.c", "selftest_cosmofs_compress", "unix.Malloc", 3),
    "fsctl-check": ("kernel-services/filesystem/cosmofs/cosmofstest.c", "selftest_fsctl_check", "unix.Malloc", 1),
    "vfs-result": ("kernel-services/vfs/vfstest.c", "selftest_fsctl_result_per_open", "unix.Malloc", 2),
    "nvme-threads": ("kernel/device/devtest.c", "selftest_nvme", "core.UndefinedBinaryOperatorResult", 1,
                     "nvme_run_workers"),
    "guestmem-copy": ("kernel-services/virtualization/guestmem.c", "copy",
                       ("core.UndefinedBinaryOperatorResult", "unix.cstring.NullArg"), 2),
    "epoll-wqs": ("kernel/io/epoll.c", "hook_item", "core.uninitialized.Assign", 1),
    "hv-uart-sibling": ("kernel-services/virtualization/hvtest.c", "selftest_el2_guest_uart_race",
                         "core.UndefinedBinaryOperatorResult", {"x86_64": 0, "aarch64": 1}),
    "kill-current": ("kernel/syscall/native.c", "kill_one", "core.NullDereference", 1),
}
# Where a case's function is now, when it has moved since the baseline: roadmap 2.0 SA7 took cosmofs_core.c apart. The
# old mode analyzes the baseline's file, where the defect was; the fixed mode analyzes the function where it now lives,
# or the fixed mode would pass on a file that no longer has the function. Once that file is CeeSafe (freelog_fill is,
# in cosmofs_commit.cee; cosmofs_test_corrupt, in cosmofs_corrupt.cee; load_root, in cosmofs_core.cee; the self-tests,
# in cosmofstest_media.cee and cosmofstest_orphan.cee) the fixed mode analyzes the generated C, which has no C function
# of the first's name and none of the others' defects, so it passes without saying anything: only the old mode still
# shows the defect.
MOVED = {
    "cosmofs-freelog": "kernel-services/filesystem/cosmofs/cosmofs_commit.c",
    "cosmofs-corrupt": "kernel-services/filesystem/cosmofs/cosmofs_corrupt.c",
    # cosmofstest.c's self-tests are CeeSafe too, by group (cosmofstest_*.cee).
    "cosmofs-badmap": "kernel-services/filesystem/cosmofs/cosmofstest_media.c",
    "cosmofs-mirror": "kernel-services/filesystem/cosmofs/cosmofstest_media.c",
    "cosmofs-compress": "kernel-services/filesystem/cosmofs/cosmofstest_media.c",
    "fsctl-check": "kernel-services/filesystem/cosmofs/cosmofstest_orphan.c",
    # vfstest.c's are CeeSafe too (vfstest_dev.cee has the per-open fsctl result).
    "vfs-result": "kernel-services/vfs/vfstest_dev.c",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("x86_64", "aarch64"), required=True)
    parser.add_argument("--case", choices=CASES, required=True)
    parser.add_argument("--old", action="store_true")
    parser.add_argument("--tree", default="HEAD")
    args = parser.parse_args()
    case = CASES[args.case]
    source, function, checker, old_count = case[:4]
    fixed_function = case[4] if len(case) > 4 else function
    work = ROOT / "out/analyzer-fix-probe" / (args.arch + ("-old" if args.old else "-fixed")) / args.case
    work.mkdir(parents=True, exist_ok=True)
    # The old mode analyzes the baseline's own tree, where the defect was. It used to overlay the baseline's file on
    # --tree, which stops working once the file is CeeSafe or taken apart (nothing builds it) or today's headers have
    # moved on (kill-current's native.c, 2026-11-08): the baseline built its file, and needs no CeeSafe compiler.
    revision = BASELINE if args.old else args.tree
    gone = args.old
    with tempfile.TemporaryDirectory(prefix="tree-", dir=work) as directory:
        tree = Path(directory) / "tree"
        subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), revision], check=True)
        try:
            out = tree / "out/probe"
            analyzed = source if args.old else MOVED.get(args.case, source)
            report = out / Path(analyzed).with_suffix(".analyzed")
            make = "gmake" if sys.platform == "darwin" else "make"
            command = [make, "-C", str(tree), "ARCH=" + args.arch, "OUT=" + str(out), str(report)]
            with (work / "build.log").open("w") as log:
                if args.old and gone:
                    # The baseline's rule prints text and touches the report; its command, run with plist output into
                    # the report, gives the findings with their functions as today's rule does.
                    dry = subprocess.run(command[:3] + ["-n", "-B"] + command[3:], capture_output=True, text=True)
                    lines = [ln for ln in dry.stdout.splitlines() if "--analyze" in ln and source in ln]
                    if len(lines) != 1:
                        print("PROBE: FAIL: the baseline's analyzer command is not one line; see " + str(work))
                        (work / "build.log").write_text(dry.stdout + dry.stderr)
                        return 1
                    report.parent.mkdir(parents=True, exist_ok=True)
                    run = lines[0].replace("-analyzer-output=text", "-analyzer-output=plist").replace(
                        "-o /dev/null", "-o " + str(report))
                    rc = subprocess.run(run, shell=True, cwd=tree, stdout=log, stderr=subprocess.STDOUT).returncode
                else:
                    rc = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT).returncode
            if rc:
                print("PROBE: FAIL: analyzer command failed; see " + str(work / "build.log"))
                return 1
            findings = plistlib.loads(report.read_bytes())["diagnostics"]
            expected_function = function if args.old else fixed_function
            if isinstance(checker, tuple):
                matches = [d for d in findings if d.get("issue_context") == expected_function
                           and d["check_name"] in checker]
            else:
                matches = [d for d in findings if d.get("issue_context") == expected_function
                           and d["check_name"].startswith(checker)]
            if isinstance(old_count, dict):
                expected = old_count[args.arch] if args.old else 0
            else:
                expected = old_count if args.old else 0
            for finding in matches:
                print(f"{source}: {expected_function}: {finding['description']} [{finding['check_name']}]")
            ok = len(matches) == expected
            print(f"PROBE: {'PASS' if ok else 'FAIL'}: {args.arch} {args.case}: "
                  f"{len(matches)} diagnostics, expected {expected} ({work})")
            return 0 if ok else 1
        finally:
            subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)], check=True)


if __name__ == "__main__":
    sys.exit(main())
