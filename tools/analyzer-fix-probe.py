#!/usr/bin/env python3
"""Check a diagnostic repair, or restore its source from main in a worktree.

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
    with tempfile.TemporaryDirectory(prefix="tree-", dir=work) as directory:
        tree = Path(directory) / "tree"
        subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), args.tree], check=True)
        try:
            if args.old:
                old = subprocess.check_output(["git", "-C", str(ROOT), "show", BASELINE + ":" + source])
                (tree / source).write_bytes(old)
            out = tree / "out/probe"
            report = out / Path(source).with_suffix(".analyzed")
            make = "gmake" if sys.platform == "darwin" else "make"
            with (work / "build.log").open("w") as log:
                rc = subprocess.run([make, "-C", str(tree), "ARCH=" + args.arch,
                                     "OUT=" + str(out), str(report)], stdout=log, stderr=subprocess.STDOUT).returncode
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
