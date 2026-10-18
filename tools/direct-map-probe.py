#!/usr/bin/env python3
"""Prove the two defects a 4 GiB machine found (docs/boot/design.md, "The
direct map"; docs/audit/2026-10-18-direct-map-report.md).

    direct-map-probe.py --defect loader|blk [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

Each run builds the debug image in a throwaway worktree and boots it with
4 GiB of RAM (tests/boot/platform_test.py, variant mem4g).

loader: the loader mapped a fixed 4 GiB, and with 4 GiB of RAM the firmware
    loads the loader above it (QEMU virt: at 0x13f...). On hardware the
    instruction after the TTBR0 switch faults and nothing follows "jumping
    to kernel entry" (Parallels Desktop's default VM; QEMU under hvf).
    Under TCG the switch sequence is already translated and the kernel
    starts anyway, so the old binary alone shows nothing here. --old
    restores the fixed size and keeps the loader's switch check, which then
    stops the boot with "the loader would lose its own code at the table
    switch": the deterministic form of the hang. Fixed: mem4g passes.

blk: a block device with no hardware behind it was held to the 32-bit
    default DMA mask, so with RAM above 4 GiB its kmalloc buffers were
    refused and blk-submit-unregister's submitter never landed a bio: the
    boot hangs at the watchdog. --old reverse-applies the fix commit
    (kernel/block/blk.c and kernel/device/devtest.c, found by its subject
    or named with --fix). Fixed: mem4g passes.

Logs stay under out/direct-map-probe/<defect>-<arch>-<mode>. Exit 0 when the
mode shows what it should, else 1.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BLK_SUBJECT = "blk: a device with no hardware takes any direct-map buffer"
BLK_SOURCES = ["kernel/block/blk.c", "kernel/device/devtest.c"]
SIZE_LINE = "                                      own, sizeof(own) / sizeof(own[0]));\n"
OLD_SIZE = SIZE_LINE + "    info->hhdm_size = 4ULL << 30;   /* direct-map-probe --old: the fixed map of v1.0.4 */\n"
LOADER_STOP = "the loader would lose its own code at the table switch"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--defect", choices=("loader", "blk"), required=True)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="aarch64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="blk: the fix commit (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "direct-map-probe" / f"{a.defect}-{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"dmp-{a.arch[:3]}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old and a.defect == "loader":
            main_c = tree / "boot" / "uefi" / "main.c"
            text = main_c.read_text()
            if text.count(SIZE_LINE) != 1:
                sys.exit("boot/uefi/main.c: the direct_map_size call is not where this probe expects it")
            main_c.write_text(text.replace(SIZE_LINE, OLD_SIZE))
        elif a.old:
            sha = a.fix or subprocess.check_output(
                ["git", "-C", str(ROOT), "log", "-1", "--format=%H", "--fixed-strings", f"--grep={BLK_SUBJECT}",
                 a.tree], text=True).strip()
            if not sha:
                sys.exit(f"no commit '{BLK_SUBJECT}' reachable from {a.tree}; name it with --fix")
            patch = subprocess.check_output(["git", "-C", str(ROOT), "show", "--format=", sha, "--"] + BLK_SOURCES)
            if not patch:
                sys.exit(f"commit {sha[:8]} does not change {BLK_SOURCES}")
            subprocess.run(["git", "-C", str(tree), "apply", "-R"], input=patch, check=True)
        make = "gmake" if sys.platform == "darwin" else "make"
        out = tree / "out" / "dbg"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", "BUILD=debug", f"OUT={out}",
                # BusyBox and the pinned sources are the same in either mode.
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}"]
        with (work / "make.log").open("w") as log:
            rc = subprocess.run(args + ["image"], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                timeout=1800).returncode
        if rc != 0:
            print(f"PROBE: FAIL arch={a.arch} defect={a.defect} mode={mode}: the build failed ({work}/make.log)")
            return 1
        boots = tree / "out" / "pt"
        res = subprocess.run([sys.executable, str(tree / "tests" / "boot" / "platform_test.py"), "--arch", a.arch,
                              "--image", str(out / "cosmoos.img"), "--workdir", str(boots), "--key", "/dev/null",
                              "--variants", "mem4g", "--timeout", "300"],
                             stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=900)
        (work / "platform_test.txt").write_text(res.stdout + res.stderr)
        serial = boots / "mem4g" / "serial.log"
        log = serial.read_text(errors="replace") if serial.exists() else ""
        if serial.exists():
            shutil.copyfile(serial, work / "serial.log")
        if not a.old:
            ok = res.returncode == 0
            seen = "mem4g passed" if ok else "mem4g failed"
        elif a.defect == "loader":
            ok = LOADER_STOP in log and "CosmoOS kernel" not in log
            seen = ("the loader stopped: its code lies outside the 4 GiB map" if ok else
                    "no such stop (the loader may be below 4 GiB on this machine)")
        else:
            ok = res.returncode != 0 and "[WATCHDOG]" in log and "'blkrace'" in log
            seen = ("the boot hung in blk-submit-unregister (watchdog, blkrace running)" if ok else
                    "no such hang")
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} defect={a.defect} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
